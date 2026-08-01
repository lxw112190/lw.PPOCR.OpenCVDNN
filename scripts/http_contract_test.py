#!/usr/bin/env python3
"""Validate live HTTP responses against the frozen OpenAPI v1 schemas."""

import argparse
import base64
import json
import os
import pathlib
import platform
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


def request(url, data=None, content_type=None, api_key=None, timeout=120):
    headers = {}
    if content_type is not None:
        headers["Content-Type"] = content_type
    if api_key is not None:
        headers["X-API-Key"] = api_key
    message = urllib.request.Request(url, data=data, headers=headers)
    try:
        with urllib.request.urlopen(message, timeout=timeout) as response:
            status = response.status
            response_headers = dict(response.headers.items())
            payload = response.read()
    except urllib.error.HTTPError as error:
        status = error.code
        response_headers = dict(error.headers.items())
        payload = error.read()
    return status, response_headers, json.loads(payload.decode("utf-8"))


def validate_headers(headers, document):
    assert headers.get("X-LW-PPOCR-API-Version") == "1"
    assert headers.get("X-Request-ID") == document.get("request_id")
    content_type = headers.get("Content-Type", "")
    assert content_type.startswith("application/json")


def main() -> int:
    configure_utf8_output()
    parser = argparse.ArgumentParser()
    parser.add_argument("--package-dir", type=pathlib.Path, required=True)
    parser.add_argument("--port", type=int, default=18782)
    args = parser.parse_args()

    package = args.package_dir.resolve()
    openapi = json.loads((package / "schemas/http-api-v1.openapi.json")
        .read_text(encoding="utf-8"))
    schemas = openapi["components"]["schemas"]
    executable = package / ("lw-ppocr-http-service.exe"
        if platform.system() == "Windows" else "lw-ppocr-http-service")
    secret = "http-contract-v1-secret"
    config = json.loads((package / "http-service.json").read_text(
        encoding="utf-8"))
    config["listen_host"] = "127.0.0.1"
    config["port"] = args.port
    config["api_key"] = secret
    config["logging"]["enabled"] = False
    config_path = package / f".http-contract-{os.getpid()}.json"
    config_path.write_text(json.dumps(
        config, ensure_ascii=False, indent=2), encoding="utf-8")

    process = None
    output = ""
    validated = []
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
                status, headers, health = request(
                    base_url + "/health", timeout=2)
                if status == 200 and health.get("ok") is True:
                    validate_headers(headers, health)
                    validate(health, schemas["HealthResponse"], openapi)
                    validated.append("health")
                    break
            except (urllib.error.URLError, TimeoutError):
                pass
            time.sleep(0.25)
        else:
            raise RuntimeError("HTTP contract service readiness timed out")
        if process.poll() is not None:
            raise RuntimeError("HTTP contract service exited before readiness")

        sample = (package / "models/ppocrv6-tiny/sample.jpg").read_bytes()
        status, headers, ocr = request(base_url + "/api/ocr", sample,
            "image/jpeg", secret)
        assert status == 200
        validate_headers(headers, ocr)
        validate(ocr, schemas["OcrResponse"], openapi)
        validated.append("ocr-binary")

        single_request = {"image_base64":
            base64.b64encode(sample).decode("ascii")}
        validate(single_request, schemas["SingleImageRequest"], openapi)
        status, headers, recognized = request(
            base_url + "/api/recognize",
            json.dumps(single_request).encode("utf-8"),
            "application/json", secret)
        assert status == 200
        validate_headers(headers, recognized)
        validate(recognized, schemas["RecognitionResponse"], openapi)
        assert "image_count" not in recognized
        validated.append("recognize-single-json")

        batch_request = {"images_base64": [
            single_request["image_base64"], single_request["image_base64"]]}
        validate(batch_request, schemas["BatchImageRequest"], openapi)
        status, headers, batch = request(
            base_url + "/api/recognize",
            json.dumps(batch_request).encode("utf-8"),
            "application/json", secret)
        assert status == 200 and batch.get("image_count") == 2
        validate_headers(headers, batch)
        validate(batch, schemas["RecognitionResponse"], openapi)
        validated.append("recognize-batch-json")

        status, headers, unauthorized = request(
            base_url + "/api/ocr", sample, "image/jpeg")
        assert status == 401 and unauthorized.get("error_code") == "unauthorized"
        validate_headers(headers, unauthorized)
        validate(unauthorized, schemas["ErrorResponse"], openapi)
        validated.append("error-unauthorized")

        status, headers, malformed = request(
            base_url + "/api/ocr", b"{", "application/json", secret)
        assert status == 400 and malformed.get("error_code") == "invalid_json"
        validate_headers(headers, malformed)
        validate(malformed, schemas["ErrorResponse"], openapi)
        validated.append("error-invalid-json")

        assert process.poll() is None
        print(json.dumps({
            "http_api_version": 1,
            "validated": validated,
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
