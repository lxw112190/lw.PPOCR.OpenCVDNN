#!/usr/bin/env python3
"""Verify split runtime/access logs, JSONL fields, privacy, and proxy trust."""

import argparse
import json
import os
import pathlib
import platform
import re
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.request

from json_schema import validate


def configure_utf8_output() -> None:
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            reconfigure(encoding="utf-8", errors="backslashreplace")


def send(url, data=None, headers=None, timeout=120):
    request = urllib.request.Request(url, data=data, headers=headers or {})
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            status = response.status
            response_headers = dict(response.headers.items())
            payload = response.read()
    except urllib.error.HTTPError as error:
        status = error.code
        response_headers = dict(error.headers.items())
        payload = error.read()
    return status, response_headers, json.loads(payload.decode("utf-8"))


def main() -> int:
    configure_utf8_output()
    parser = argparse.ArgumentParser()
    parser.add_argument("--package-dir", type=pathlib.Path, required=True)
    parser.add_argument("--port", type=int, default=18785)
    parser.add_argument(
        "--access-format", choices=("jsonl", "text"), default="jsonl")
    args = parser.parse_args()

    package = args.package_dir.resolve()
    executable = package / ("lw-ppocr-http-service.exe"
        if platform.system() == "Windows" else "lw-ppocr-http-service")
    secret = "logging-smoke-secret-must-not-leak"
    log_directory = package / f".logging-smoke-{os.getpid()}"
    runtime_log = log_directory / "runtime.log"
    access_log = log_directory / "access.log"
    config_path = package / f".logging-smoke-{os.getpid()}.json"
    config = json.loads((package / "http-service.json").read_text(
        encoding="utf-8"))
    config["listen_host"] = "127.0.0.1"
    config["port"] = args.port
    config["api_key"] = secret
    config["worker_threads"] = 2
    config["logging"] = {
        "enabled": True,
        "level": "info",
        "console": False,
        "file_enabled": True,
        "file_path": log_directory.name + "/runtime.log",
        "request_enabled": True,
        "request_start_enabled": True,
        "access_file_enabled": True,
        "access_file_path": log_directory.name + "/access.log",
        "access_format": args.access_format,
        "flush_interval_seconds": 1,
        "trusted_proxies": ["127.0.0.1", "::1"],
        "max_file_size_mb": 2,
        "max_files": 2,
    }
    config_path.write_text(json.dumps(
        config, ensure_ascii=False, indent=2), encoding="utf-8")

    process = None
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
                status, _, health = send(base_url + "/health", timeout=2)
                if status == 200 and health.get("ok") is True:
                    break
            except (urllib.error.URLError, TimeoutError):
                pass
            time.sleep(0.25)
        else:
            raise RuntimeError("logging smoke service readiness timed out")
        if process.poll() is not None:
            raise RuntimeError("logging smoke service exited before readiness")

        image = (package / "models/ppocrv6-tiny/sample.jpg").read_bytes()
        common_headers = {
            "Content-Type": "image/jpeg",
            "X-API-Key": secret,
            "X-Forwarded-For": "203.0.113.9, 127.0.0.1",
        }
        status, response_headers, result = send(
            base_url + "/api/ocr", image, common_headers)
        assert status == 200 and result["ok"] is True
        assert response_headers.get("X-Request-ID") == result["request_id"]

        status, response_headers, unauthorized = send(
            base_url + "/api/ocr", image, {
                "Content-Type": "image/jpeg",
                "X-API-Key": "wrong-" + secret,
            })
        assert status == 401 and unauthorized["ok"] is False
        assert response_headers.get("X-Request-ID") == unauthorized["request_id"]

        status, response_headers, invalid = send(
            base_url + "/api/ocr", b"not-an-image", common_headers)
        assert status == 400 and invalid["ok"] is False
        assert response_headers.get("X-Request-ID") == invalid["request_id"]

        time.sleep(1.5)
        runtime_text = runtime_log.read_text(encoding="utf-8")
        access_text = access_log.read_text(encoding="utf-8")
        lines = [line for line in access_text.splitlines() if line.strip()]
        assert len(lines) == 3
        if args.access_format == "jsonl":
            records = [json.loads(line) for line in lines]
            access_schema = json.loads((
                package / "schemas/access-log-v1.schema.json").read_text(
                    encoding="utf-8"))
            for record in records:
                validate(record, access_schema)
            assert [record["status"] for record in records] == [200, 401, 400]
            assert all(record["log_schema_version"] == 1 for record in records)
            assert all(record["event"] == "request_complete" for record in records)
            assert all(record["timestamp"].endswith("Z") for record in records)
            assert records[0]["remote_ip"] == "203.0.113.9"
            assert records[0]["peer_ip"] in {"127.0.0.1", "::1"}
            assert records[0]["request_format"] == "binary"
            assert records[0]["operation"] == "ocr"
            assert records[0]["image"] == {"width": 500, "height": 500}
            assert records[0]["result_count"] > 0
            assert "detector" in records[0]["timing_ms"]
            assert records[1]["error_code"] == "unauthorized"
            assert records[2]["error_code"] == "invalid_request"
        else:
            prefix = re.compile(
                r"^\[\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{3}\] "
                r"\[info\] \[\d+\] request_id=\S+")
            assert all(prefix.search(line) for line in lines)
            assert [f"status={status}" in line
                for line, status in zip(lines, (200, 401, 400))] == [True] * 3
            assert "remote_ip=203.0.113.9" in lines[0]
            assert "error_code=unauthorized" in lines[1]
            assert "error_code=invalid_request" in lines[2]

        combined = runtime_text + access_text
        # Unauthorized traffic must not force a synchronous runtime-log flush.
        assert runtime_text.count("request_started") == 2
        assert secret not in combined
        assert "image_base64" not in combined
        assert "纯臻营养护发素" not in combined
        print(json.dumps({
            "access_format": args.access_format,
            "access_records": len(lines),
            "statuses": [200, 401, 400],
            "trusted_proxy_remote_ip": "203.0.113.9",
            "request_id_header": "passed",
            "privacy_checks": "passed",
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
        try:
            config_path.unlink()
        except FileNotFoundError:
            pass
        shutil.rmtree(log_directory, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
