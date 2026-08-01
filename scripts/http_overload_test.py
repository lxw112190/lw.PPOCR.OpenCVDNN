#!/usr/bin/env python3
"""Verify bounded engine admission, timeout responses, and recovery."""

import argparse
import concurrent.futures
import json
import os
import pathlib
import platform
import struct
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request


def make_bmp(width: int, height: int) -> bytes:
    row_size = (width * 3 + 3) & ~3
    row = b"\xf5\xf5\xf5" * width + bytes(row_size - width * 3)
    body_size = row_size * height
    file_header = struct.pack("<2sIHHI", b"BM", 54 + body_size, 0, 0, 54)
    info_header = struct.pack(
        "<IIIHHIIIIII", 40, width, height, 1, 24, 0, body_size,
        2835, 2835, 0, 0)
    return file_header + info_header + row * height


def configure_utf8_output() -> None:
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            reconfigure(encoding="utf-8", errors="backslashreplace")


def request(url: str, data=None, content_type=None, timeout=120):
    headers = {}
    if content_type:
        headers["Content-Type"] = content_type
    message = urllib.request.Request(url, data=data, headers=headers)
    try:
        with urllib.request.urlopen(message, timeout=timeout) as response:
            return response.status, dict(response.headers.items()), json.loads(
                response.read().decode("utf-8"))
    except urllib.error.HTTPError as error:
        payload = error.read()
        return error.code, dict(error.headers.items()), (
            json.loads(payload.decode("utf-8")) if payload else {})
    except (urllib.error.URLError, TimeoutError, ConnectionError) as error:
        return 0, {}, {"transport_error": str(error)}


def main() -> int:
    configure_utf8_output()
    parser = argparse.ArgumentParser()
    parser.add_argument("--package-dir", type=pathlib.Path, required=True)
    parser.add_argument("--port", type=int, default=18781)
    args = parser.parse_args()

    package = args.package_dir.resolve()
    executable = package / ("lw-ppocr-http-service.exe"
        if platform.system() == "Windows" else "lw-ppocr-http-service")
    config = json.loads((package / "http-service.json").read_text(
        encoding="utf-8"))
    config.update({
        "listen_host": "127.0.0.1",
        "port": args.port,
        "api_key": "",
        "engine_instances": 1,
        "worker_threads": 4,
        "max_queued_requests": 1,
        "engine_wait_timeout_ms": 100,
    })
    config["logging"]["enabled"] = False
    config_path = package / f".http-overload-{os.getpid()}.json"
    config_path.write_text(json.dumps(
        config, ensure_ascii=False, indent=2), encoding="utf-8")

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
                status, _, health = request(base_url + "/health", timeout=2)
                if status == 200 and health.get("ok") is True:
                    break
            except Exception:
                pass
            time.sleep(0.25)
        else:
            raise RuntimeError("overload test service readiness timed out")
        if process.poll() is not None:
            raise RuntimeError("overload test service exited before readiness")

        sample = make_bmp(2400, 1800)
        endpoint = base_url + "/api/ocr"
        observed = []
        for _ in range(6):
            barrier = threading.Barrier(8)

            def send_one():
                barrier.wait(timeout=10)
                return request(endpoint, sample, "image/bmp")

            with concurrent.futures.ThreadPoolExecutor(
                    max_workers=8) as executor:
                futures = [executor.submit(send_one) for _ in range(8)]
                observed.extend(future.result() for future in futures)
            codes = {item[2].get("error_code") for item in observed}
            if "queue_full" in codes and "engine_wait_timeout" in codes:
                break

        overload = [item for item in observed if item[0] in (429, 503)]
        assert overload, f"no overload response observed: {observed}"
        codes = {item[2].get("error_code") for item in overload}
        assert "queue_full" in codes, f"queue_full was not observed: {observed}"
        assert "engine_wait_timeout" in codes, (
            f"engine_wait_timeout was not observed: {observed}")
        for status, headers, document in overload:
            assert document.get("ok") is False
            assert headers.get("X-Request-ID") == document.get("request_id")
            assert headers.get("Retry-After") == "1"
            assert (status, document.get("error_code")) in {
                (429, "queue_full"), (503, "engine_wait_timeout")}

        status, _, recovered = request(
            endpoint, sample, "image/bmp", timeout=120)
        assert status == 200 and recovered.get("ok") is True
        assert process.poll() is None
        print(json.dumps({
            "requests": len(observed),
            "queue_full": sum(item[0] == 429 for item in observed),
            "engine_wait_timeout": sum(item[0] == 503 for item in observed),
            "transport_rejections": sum(item[0] == 0 for item in observed),
            "post_overload_recovery": "passed",
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
        try:
            config_path.unlink()
        except FileNotFoundError:
            pass


if __name__ == "__main__":
    raise SystemExit(main())
