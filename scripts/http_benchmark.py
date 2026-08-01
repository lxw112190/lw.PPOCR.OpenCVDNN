#!/usr/bin/env python3
"""Benchmark a packaged lw.PPOCR.OpenCVDNN HTTP service reproducibly."""

import argparse
import concurrent.futures
import ctypes
import http.client
import json
import os
import pathlib
import platform
import statistics
import subprocess
import sys
import threading
import time


def configure_utf8_output() -> None:
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            reconfigure(encoding="utf-8", errors="backslashreplace")


def percentile(values, percentage):
    ordered = sorted(values)
    if not ordered:
        return 0.0
    position = (len(ordered) - 1) * percentage / 100.0
    lower = int(position)
    upper = min(lower + 1, len(ordered) - 1)
    fraction = position - lower
    return ordered[lower] * (1.0 - fraction) + ordered[upper] * fraction


def process_rss_bytes(pid: int) -> int:
    system = platform.system()
    if system == "Linux":
        status = pathlib.Path(f"/proc/{pid}/status").read_text(
            encoding="utf-8")
        for line in status.splitlines():
            if line.startswith("VmRSS:"):
                return int(line.split()[1]) * 1024
    elif system == "Darwin":
        result = subprocess.run(
            ["ps", "-o", "rss=", "-p", str(pid)], check=True,
            capture_output=True, text=True)
        return int(result.stdout.strip()) * 1024
    elif system == "Windows":
        class ProcessMemoryCounters(ctypes.Structure):
            _fields_ = [
                ("cb", ctypes.c_ulong),
                ("PageFaultCount", ctypes.c_ulong),
                ("PeakWorkingSetSize", ctypes.c_size_t),
                ("WorkingSetSize", ctypes.c_size_t),
                ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                ("PagefileUsage", ctypes.c_size_t),
                ("PeakPagefileUsage", ctypes.c_size_t),
            ]
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        psapi = ctypes.WinDLL("psapi", use_last_error=True)
        kernel32.OpenProcess.argtypes = [
            ctypes.c_ulong, ctypes.c_int, ctypes.c_ulong]
        kernel32.OpenProcess.restype = ctypes.c_void_p
        kernel32.CloseHandle.argtypes = [ctypes.c_void_p]
        psapi.GetProcessMemoryInfo.argtypes = [
            ctypes.c_void_p, ctypes.POINTER(ProcessMemoryCounters),
            ctypes.c_ulong]
        handle = kernel32.OpenProcess(0x0400 | 0x0010, False, pid)
        if not handle:
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            counters = ProcessMemoryCounters()
            counters.cb = ctypes.sizeof(counters)
            if not psapi.GetProcessMemoryInfo(
                    handle, ctypes.byref(counters), counters.cb):
                raise ctypes.WinError(ctypes.get_last_error())
            return int(counters.WorkingSetSize)
        finally:
            kernel32.CloseHandle(handle)
    raise RuntimeError(f"RSS measurement is unsupported on {system}")


def parse_matrix(value):
    result = []
    for item in value.split(","):
        engine_text, concurrency_text = item.split(":", 1)
        engines = int(engine_text)
        concurrency = int(concurrency_text)
        if engines < 1 or concurrency < 1:
            raise ValueError("matrix values must be positive")
        result.append((engines, concurrency))
    return result


class Connections:
    def __init__(self, host, port, timeout):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.local = threading.local()

    def get(self):
        connection = getattr(self.local, "connection", None)
        if connection is None:
            connection = http.client.HTTPConnection(
                self.host, self.port, timeout=self.timeout)
            self.local.connection = connection
        return connection

    def request(self, path, body, content_type):
        started = time.perf_counter()
        for attempt in range(2):
            connection = self.get()
            try:
                connection.request("POST", path, body=body, headers={
                    "Content-Type": content_type,
                    "Connection": "keep-alive",
                })
                response = connection.getresponse()
                payload = response.read()
                break
            except (ConnectionError, http.client.HTTPException, OSError):
                connection.close()
                self.local.connection = None
                if attempt != 0:
                    raise
        elapsed_ms = (time.perf_counter() - started) * 1000.0
        document = json.loads(payload.decode("utf-8"))
        if response.status != 200 or document.get("ok") is not True:
            raise RuntimeError(
                f"HTTP benchmark request failed: {response.status} {document}")
        server_ms = float(document["timing"]["server_total_ms"])
        return elapsed_ms, server_ms, len(document.get("result", []))


def health(port):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=2)
    try:
        connection.request("GET", "/health")
        response = connection.getresponse()
        payload = response.read()
        return response.status == 200 and json.loads(
            payload.decode("utf-8")).get("ok") is True
    finally:
        connection.close()


def summarize(client_ms, server_ms, elapsed_seconds, rss_values):
    count = len(client_ms)
    return {
        "requests": count,
        "throughput_rps": round(count / elapsed_seconds, 3),
        "client_latency_ms": {
            "mean": round(statistics.fmean(client_ms), 3),
            "p50": round(percentile(client_ms, 50), 3),
            "p95": round(percentile(client_ms, 95), 3),
            "p99": round(percentile(client_ms, 99), 3),
            "max": round(max(client_ms), 3),
        },
        "server_total_ms": {
            "mean": round(statistics.fmean(server_ms), 3),
            "p50": round(percentile(server_ms, 50), 3),
            "p95": round(percentile(server_ms, 95), 3),
            "p99": round(percentile(server_ms, 99), 3),
            "max": round(max(server_ms), 3),
        },
        "rss_mb": {
            "start": round(rss_values[0] / 1048576, 2),
            "end": round(rss_values[-1] / 1048576, 2),
            "peak": round(max(rss_values) / 1048576, 2),
        },
    }


def benchmark_workload(
        connections, process, endpoint, body, content_type,
        warmup, iterations, concurrency):
    with concurrent.futures.ThreadPoolExecutor(
            max_workers=concurrency) as executor:
        warmup_futures = [executor.submit(
            connections.request, endpoint, body, content_type)
            for _ in range(max(warmup, concurrency))]
        for future in concurrent.futures.as_completed(warmup_futures):
            future.result()
    rss_values = [process_rss_bytes(process.pid)]
    client_ms = []
    server_ms = []
    started = time.perf_counter()
    with concurrent.futures.ThreadPoolExecutor(
            max_workers=concurrency) as executor:
        futures = [executor.submit(
            connections.request, endpoint, body, content_type)
            for _ in range(iterations)]
        for future in concurrent.futures.as_completed(futures):
            client_value, server_value, _ = future.result()
            client_ms.append(client_value)
            server_ms.append(server_value)
            rss_values.append(process_rss_bytes(process.pid))
    elapsed_seconds = time.perf_counter() - started
    time.sleep(0.25)
    rss_values.append(process_rss_bytes(process.pid))
    return summarize(client_ms, server_ms, elapsed_seconds, rss_values)


def run_scenario(package, port, engines, concurrency, warmup, iterations):
    executable = package / ("lw-ppocr-http-service.exe"
        if platform.system() == "Windows" else "lw-ppocr-http-service")
    config = json.loads((package / "http-service.json").read_text(
        encoding="utf-8"))
    config.update({
        "listen_host": "127.0.0.1",
        "port": port,
        "api_key": "",
        "engine_instances": engines,
        "worker_threads": max(4, concurrency),
        "max_queued_requests": 32,
        "engine_wait_timeout_ms": 120000,
    })
    config["logging"]["enabled"] = False
    config_path = package / (
        f".http-benchmark-{os.getpid()}-{engines}-{concurrency}.json")
    config_path.write_text(json.dumps(
        config, ensure_ascii=False, indent=2), encoding="utf-8")
    process = None
    output = ""
    try:
        launch_started = time.perf_counter()
        process = subprocess.Popen(
            [str(executable), "--config", str(config_path)], cwd=package,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
            encoding="utf-8", errors="replace")
        for _ in range(240):
            if process.poll() is not None:
                break
            try:
                if health(port):
                    break
            except Exception:
                pass
            time.sleep(0.1)
        else:
            raise RuntimeError("benchmark service readiness timed out")
        if process.poll() is not None:
            raise RuntimeError("benchmark service exited before readiness")
        startup_ms = (time.perf_counter() - launch_started) * 1000.0
        initialized_rss = process_rss_bytes(process.pid)

        workloads = [
            ("full_ocr_sample_500x500", "/api/ocr",
             (package / "models/ppocrv6-tiny/sample.jpg").read_bytes(),
             "image/jpeg"),
            ("recognize_headline_crop", "/api/recognize",
             (package / "tests/fixtures/ppocrv6-tiny/headline.png").read_bytes(),
             "image/png"),
        ]
        connections = Connections("127.0.0.1", port, 180)
        results = {}
        for name, endpoint, body, content_type in workloads:
            results[name] = benchmark_workload(
                connections, process, endpoint, body, content_type,
                warmup, iterations, concurrency)
        return {
            "engine_instances": engines,
            "concurrency": concurrency,
            "worker_threads": max(4, concurrency),
            "startup_ms": round(startup_ms, 3),
            "initialized_rss_mb": round(initialized_rss / 1048576, 2),
            "workloads": results,
        }
    finally:
        if process is not None:
            process.terminate()
            try:
                process.wait(timeout=20)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
            if process.stdout:
                output = process.stdout.read()
            if process.returncode not in (0, -15, 1):
                print(output, file=sys.stderr)
        try:
            config_path.unlink()
        except FileNotFoundError:
            pass


def main() -> int:
    configure_utf8_output()
    parser = argparse.ArgumentParser()
    parser.add_argument("--package-dir", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--port", type=int, default=18900)
    parser.add_argument("--warmup", type=int, default=10)
    parser.add_argument("--iterations", type=int, default=100)
    parser.add_argument("--matrix", default="1:1,1:4,2:2,2:4,4:4")
    args = parser.parse_args()
    if args.warmup < 1 or args.iterations < 1:
        parser.error("warmup and iterations must be positive")
    package = args.package_dir.resolve()
    matrix = parse_matrix(args.matrix)
    scenarios = []
    for index, (engines, concurrency) in enumerate(matrix):
        print(f"benchmarking engines={engines}, concurrency={concurrency}",
              flush=True)
        scenarios.append(run_scenario(
            package, args.port + index, engines, concurrency,
            args.warmup, args.iterations))
    document = {
        "benchmark_schema_version": 1,
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "platform": {
            "system": platform.system(),
            "release": platform.release(),
            "version": platform.version(),
            "machine": platform.machine(),
            "logical_cpus": os.cpu_count(),
            "python": platform.python_version(),
        },
        "package": str(package),
        "transport": "HTTP keep-alive binary body over 127.0.0.1",
        "warmup_per_workload": args.warmup,
        "measured_requests_per_workload": args.iterations,
        "scenarios": scenarios,
    }
    rendered = json.dumps(document, ensure_ascii=False, indent=2)
    if args.output:
        output = args.output.resolve()
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(rendered + "\n", encoding="utf-8")
        print(f"Created {output}")
    print(rendered)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
