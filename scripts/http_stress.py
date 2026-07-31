#!/usr/bin/env python3
"""Exercise packaged HTTP OCR with varied images, concurrency, and RSS checks."""

import argparse
import base64
import concurrent.futures
import ctypes
import json
import os
import pathlib
import platform
import struct
import subprocess
import sys
import time
import urllib.error
import urllib.request
import zlib


def configure_utf8_output() -> None:
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            reconfigure(encoding="utf-8", errors="backslashreplace")


def request(url: str, data=None, content_type=None, timeout=120):
    message = urllib.request.Request(url, data=data)
    if content_type:
        message.add_header("Content-Type", content_type)
    try:
        with urllib.request.urlopen(message, timeout=timeout) as response:
            status = response.status
            payload = response.read()
    except urllib.error.HTTPError as error:
        status = error.code
        payload = error.read()
    document = json.loads(payload.decode("utf-8")) if payload else {}
    return status, document


def post_json(url: str, value):
    return request(url, json.dumps(value).encode("utf-8"), "application/json")


def post_binary(url: str, image: bytes, content_type="application/octet-stream"):
    return request(url, image, content_type)


def make_bmp(width: int, height: int) -> bytes:
    row_size = (width * 3 + 3) & ~3
    row = b"\xf5\xf5\xf5" * width + bytes(row_size - width * 3)
    body_size = row_size * height
    file_header = struct.pack("<2sIHHI", b"BM", 54 + body_size, 0, 0, 54)
    info_header = struct.pack(
        "<IIIHHIIIIII", 40, width, height, 1, 24, 0, body_size,
        2835, 2835, 0, 0)
    return file_header + info_header + row * height


def png_chunk(kind: bytes, data: bytes) -> bytes:
    return (struct.pack(">I", len(data)) + kind + data +
            struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF))


def make_solid_png(width: int, height: int) -> bytes:
    compressor = zlib.compressobj(level=9)
    row = b"\0" + b"\x80\x80\x80" * width
    compressed = []
    for _ in range(height):
        value = compressor.compress(row)
        if value:
            compressed.append(value)
    compressed.append(compressor.flush())
    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", header) +
            png_chunk(b"IDAT", b"".join(compressed)) + png_chunk(b"IEND", b""))


def process_rss_bytes(pid: int) -> int:
    system = platform.system()
    if system == "Linux":
        status = pathlib.Path(f"/proc/{pid}/status").read_text(encoding="utf-8")
        for line in status.splitlines():
            if line.startswith("VmRSS:"):
                return int(line.split()[1]) * 1024
        raise RuntimeError("VmRSS is missing")
    if system == "Darwin":
        result = subprocess.run(
            ["ps", "-o", "rss=", "-p", str(pid)], check=True,
            capture_output=True, text=True)
        return int(result.stdout.strip()) * 1024
    if system == "Windows":
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
        kernel32.CloseHandle.restype = ctypes.c_int
        psapi.GetProcessMemoryInfo.argtypes = [
            ctypes.c_void_p, ctypes.POINTER(ProcessMemoryCounters), ctypes.c_ulong]
        psapi.GetProcessMemoryInfo.restype = ctypes.c_int
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


def assert_success(document, require_box: bool) -> None:
    assert document.get("ok") is True
    assert isinstance(document.get("result"), list)
    assert isinstance(document.get("timing"), dict)
    if require_box:
        for item in document["result"]:
            assert isinstance(item.get("box"), list) and len(item["box"]) == 4
            assert not any(f"{axis}{index}" in item
                for axis in ("x", "y") for index in range(1, 5))


def expect_error(name: str, expected: int, response) -> str:
    status, document = response
    assert status == expected, f"{name}: expected {expected}, got {status}: {document}"
    assert document.get("ok") is False
    assert document.get("error")
    return name


def main() -> int:
    configure_utf8_output()
    parser = argparse.ArgumentParser()
    parser.add_argument("--package-dir", type=pathlib.Path, required=True)
    parser.add_argument("--port", type=int, default=18788)
    parser.add_argument("--iterations", type=int, default=64)
    parser.add_argument("--concurrency", type=int, default=2)
    parser.add_argument("--max-rss-growth-mb", type=float, default=128.0)
    args = parser.parse_args()
    if args.iterations < 1 or args.concurrency < 1:
        parser.error("iterations and concurrency must be positive")

    package = args.package_dir.resolve()
    executable = package / ("lw-ppocr-http-service.exe"
        if platform.system() == "Windows" else "lw-ppocr-http-service")
    config = json.loads((package / "http-service.json").read_text(encoding="utf-8"))
    config["port"] = args.port
    config["api_key"] = ""
    config["worker_threads"] = max(2, args.concurrency)
    config["max_request_bytes"] = 12 * 1024 * 1024
    config["max_image_pixels"] = 4_000_000
    config["logging"] = {
        "enabled": False,
        "level": "info",
        "console": False,
        "file_enabled": False,
        "file_path": "logs/http-stress.log",
        "request_enabled": False,
        "max_file_size_mb": 1,
        "max_files": 1,
    }
    config_path = package / f".http-stress-{os.getpid()}.json"
    config_path.write_text(
        json.dumps(config, ensure_ascii=False, indent=2), encoding="utf-8")

    process = None
    output = ""
    try:
        process = subprocess.Popen(
            [str(executable), "--config", str(config_path)], cwd=package,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
            encoding="utf-8", errors="replace")
        base_url = f"http://127.0.0.1:{args.port}"
        for _ in range(160):
            if process.poll() is not None:
                break
            try:
                status, health = request(f"{base_url}/health", timeout=2)
                if status == 200 and health.get("ok"):
                    break
            except (urllib.error.URLError, TimeoutError):
                pass
            time.sleep(0.25)
        else:
            raise RuntimeError("service readiness timed out")
        if process.poll() is not None:
            raise RuntimeError("service exited before readiness")

        sample = (package / "models/ppocrv6-tiny/sample.jpg").read_bytes()
        images = [
            ("tiny-64x32", make_bmp(64, 32), "image/bmp"),
            ("small-320x180", make_bmp(320, 180), "image/bmp"),
            ("medium-960x540", make_bmp(960, 540), "image/bmp"),
            ("large-2048x1536", make_bmp(2048, 1536), "image/bmp"),
        ]

        status, document = post_binary(f"{base_url}/api/ocr", sample, "image/jpeg")
        assert status == 200
        assert_success(document, True)
        for _, image, content_type in images:
            status, document = post_binary(
                f"{base_url}/api/ocr", image, content_type)
            assert status == 200
            assert_success(document, True)
        status, document = post_binary(
            f"{base_url}/api/recognize", sample, "application/octet-stream")
        assert status == 200
        assert_success(document, False)

        baseline_readings = []
        for _ in range(3):
            baseline_readings.append(process_rss_bytes(process.pid))
            time.sleep(0.2)
        baseline_rss = max(baseline_readings)
        rss_readings = list(baseline_readings)

        def run_one(index: int):
            if index % 4 == 0:
                status_value, result = post_binary(
                    f"{base_url}/api/recognize", sample, "image/jpeg")
                require_box = False
                image_name = "sample-recognize"
            else:
                image_name, image, content_type = images[index % len(images)]
                status_value, result = post_binary(
                    f"{base_url}/api/ocr", image, content_type)
                require_box = True
            assert status_value == 200
            assert_success(result, require_box)
            return image_name

        completed_images = []
        with concurrent.futures.ThreadPoolExecutor(
                max_workers=args.concurrency) as executor:
            futures = [executor.submit(run_one, index)
                for index in range(args.iterations)]
            for future in concurrent.futures.as_completed(futures):
                completed_images.append(future.result())
                rss_readings.append(process_rss_bytes(process.pid))

        invalid_cases = []
        invalid_cases.append(expect_error("empty-binary", 400,
            post_binary(f"{base_url}/api/ocr", b"", "image/jpeg")))
        invalid_cases.append(expect_error("corrupt-image", 400,
            post_binary(f"{base_url}/api/ocr", b"not an image")))
        invalid_cases.append(expect_error("invalid-json", 400,
            request(f"{base_url}/api/ocr", b"{", "application/json")))
        invalid_cases.append(expect_error("json-array", 400,
            post_json(f"{base_url}/api/ocr", [])))
        invalid_cases.append(expect_error("missing-image", 400,
            post_json(f"{base_url}/api/ocr", {})))
        invalid_cases.append(expect_error("invalid-base64", 400,
            post_json(f"{base_url}/api/ocr", {"image_base64": "%%%"})))
        invalid_cases.append(expect_error("empty-batch", 400,
            post_json(f"{base_url}/api/recognize", {"images_base64": []})))
        invalid_cases.append(expect_error("oversized-batch", 400,
            post_json(f"{base_url}/api/recognize",
                {"images_base64": [""] * 257})))
        invalid_cases.append(expect_error("oversized-request", 413,
            post_binary(f"{base_url}/api/ocr",
                b"x" * (config["max_request_bytes"] + 1))))
        invalid_cases.append(expect_error("pixel-limit", 400,
            post_binary(f"{base_url}/api/ocr",
                make_solid_png(2400, 1800), "image/png")))

        for _ in range(3):
            status, document = post_binary(
                f"{base_url}/api/ocr", sample, "image/jpeg")
            assert status == 200
            assert_success(document, True)
        time.sleep(0.5)
        end_rss = process_rss_bytes(process.pid)
        rss_readings.append(end_rss)
        growth = max(0, end_rss - baseline_rss)
        allowed_growth = int(args.max_rss_growth_mb * 1024 * 1024)
        assert growth <= allowed_growth, (
            f"RSS grew by {growth / 1048576:.2f} MiB; "
            f"limit is {args.max_rss_growth_mb:.2f} MiB")
        assert process.poll() is None

        print(json.dumps({
            "iterations": args.iterations,
            "concurrency": args.concurrency,
            "image_sizes": [name for name, _, _ in images],
            "completed_requests": len(completed_images),
            "exception_cases": invalid_cases,
            "rss_baseline_mb": round(baseline_rss / 1048576, 2),
            "rss_end_mb": round(end_rss / 1048576, 2),
            "rss_peak_mb": round(max(rss_readings) / 1048576, 2),
            "rss_growth_mb": round(growth / 1048576, 2),
            "rss_growth_limit_mb": args.max_rss_growth_mb,
            "status": "passed",
        }, ensure_ascii=False, indent=2))
        return 0
    finally:
        if process is not None:
            process.terminate()
            try:
                process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
            if process.stdout:
                output = process.stdout.read()
            if output and process.returncode not in (0, -15, 1):
                print("--- service output ---")
                print(output)
        config_path.unlink(missing_ok=True)


if __name__ == "__main__":
    raise SystemExit(main())
