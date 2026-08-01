#!/usr/bin/env python3
"""Exercise malformed and hostile HTTP inputs and verify service recovery."""

import argparse
import base64
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


def request(url, data=None, content_type=None, api_key=None, timeout=60):
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
    document = json.loads(payload.decode("utf-8")) if payload else {}
    return status, response_headers, document


def post_json(url, value, api_key):
    return request(url, json.dumps(value).encode("utf-8"),
        "application/json", api_key)


def png_chunk(kind: bytes, data: bytes) -> bytes:
    return (struct.pack(">I", len(data)) + kind + data +
            struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF))


def make_solid_png(width: int, height: int) -> bytes:
    compressor = zlib.compressobj(level=9)
    row = b"\0" + b"\x80\x80\x80" * width
    chunks = []
    for _ in range(height):
        encoded = compressor.compress(row)
        if encoded:
            chunks.append(encoded)
    chunks.append(compressor.flush())
    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", header) +
            png_chunk(b"IDAT", b"".join(chunks)) + png_chunk(b"IEND", b""))


def expect_error(name, expected_status, expected_code, response):
    status, headers, document = response
    assert status == expected_status, (
        f"{name}: expected HTTP {expected_status}, got {status}: {document}")
    assert document.get("ok") is False, f"{name}: missing ok=false"
    assert isinstance(document.get("error"), str) and document["error"], (
        f"{name}: missing error message")
    assert document.get("error_code") == expected_code, (
        f"{name}: expected error_code={expected_code}, got {document}")
    assert headers.get("X-Request-ID") == document.get("request_id"), (
        f"{name}: request ID mismatch")
    return name


def main() -> int:
    configure_utf8_output()
    parser = argparse.ArgumentParser()
    parser.add_argument("--package-dir", type=pathlib.Path, required=True)
    parser.add_argument("--port", type=int, default=18784)
    args = parser.parse_args()

    package = args.package_dir.resolve()
    executable = package / ("lw-ppocr-http-service.exe"
        if platform.system() == "Windows" else "lw-ppocr-http-service")
    secret = "invalid-input-suite-secret"
    config = json.loads((package / "http-service.json").read_text(
        encoding="utf-8"))
    config["listen_host"] = "127.0.0.1"
    config["port"] = args.port
    config["api_key"] = secret
    config["worker_threads"] = 2
    config["max_request_bytes"] = 1024 * 1024
    config["max_image_pixels"] = 1_000_000
    config["max_batch_images"] = 4
    config["max_batch_total_pixels"] = 1_000_000
    config["max_batch_decoded_bytes"] = 3_000_000
    config["logging"] = {
        "enabled": False,
        "level": "info",
        "console": False,
        "file_enabled": False,
        "file_path": "logs/invalid-input-runtime.log",
        "request_enabled": False,
        "request_start_enabled": False,
        "access_file_enabled": False,
        "access_file_path": "logs/invalid-input-access.log",
        "access_format": "text",
        "flush_interval_seconds": 1,
        "trusted_proxies": [],
        "max_file_size_mb": 1,
        "max_files": 1,
    }
    config_path = package / f".http-invalid-inputs-{os.getpid()}.json"
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
            except (urllib.error.URLError, TimeoutError):
                pass
            time.sleep(0.25)
        else:
            raise RuntimeError("invalid-input service readiness timed out")
        if process.poll() is not None:
            raise RuntimeError("invalid-input service exited before readiness")

        ocr = base_url + "/api/ocr"
        recognize = base_url + "/api/recognize"
        invalid = []
        invalid.append(expect_error("missing-api-key", 401, "unauthorized",
            request(ocr, b"x", "image/jpeg")))
        invalid.append(expect_error("wrong-api-key", 401, "unauthorized",
            request(ocr, b"x", "image/jpeg", "wrong")))

        binary_cases = [
            ("empty-binary", b"", "image/jpeg"),
            ("random-binary", b"not an image", "application/octet-stream"),
            ("truncated-jpeg", b"\xff\xd8\xff", "image/jpeg"),
            ("truncated-png", b"\x89PNG\r\n\x1a\n", "image/png"),
            ("truncated-bmp", b"BM", "image/bmp"),
            ("truncated-gif", b"GIF89a", "image/gif"),
            ("truncated-webp", b"RIFF\x00\x00\x00\x00WEBP", "image/webp"),
            ("truncated-tiff", b"II\x2a\x00", "image/tiff"),
            ("svg-is-not-raster", b"<svg/>", "image/svg+xml"),
        ]
        for name, payload, content_type in binary_cases:
            invalid.append(expect_error(name, 400, "invalid_request",
                request(ocr, payload, content_type, secret)))

        malformed_json = [b"", b"{", b"[", b'"unterminated', b"\xff"]
        for index, payload in enumerate(malformed_json):
            invalid.append(expect_error(
                f"malformed-json-{index}", 400, "invalid_json",
                request(ocr, payload, "application/json", secret)))

        wrong_documents = [None, [], "image", 42, True]
        for index, value in enumerate(wrong_documents):
            invalid.append(expect_error(
                f"json-root-type-{index}", 400, "invalid_request",
                post_json(ocr, value, secret)))

        wrong_image_values = [None, 7, True, [], {}]
        for index, value in enumerate(wrong_image_values):
            invalid.append(expect_error(
                f"image-base64-type-{index}", 400, "invalid_request",
                post_json(ocr, {"image_base64": value}, secret)))

        invalid.append(expect_error("single-extra-property", 400,
            "invalid_request", post_json(ocr,
                {"image_base64": "AAAA", "extra": True}, secret)))
        invalid.append(expect_error("recognize-single-extra-property", 400,
            "invalid_request", post_json(recognize,
                {"image_base64": "AAAA", "extra": True}, secret)))
        invalid.append(expect_error("recognize-ambiguous-shape", 400,
            "invalid_request", post_json(recognize,
                {"image_base64": "AAAA", "images_base64": ["AAAA"]},
                secret)))

        for name, value in [
                ("empty-base64", ""),
                ("invalid-base64-alphabet", "%%%"),
                ("invalid-base64-padding", "AAAA="),
                ("invalid-data-url", "data:image/png,AAAA"),
                ("decoded-corrupt-image", base64.b64encode(
                    b"not an image").decode("ascii"))]:
            invalid.append(expect_error(name, 400, "invalid_request",
                post_json(ocr, {"image_base64": value}, secret)))

        batch_cases = [
            ("batch-null", None),
            ("batch-string", "value"),
            ("batch-empty", []),
            ("batch-non-string", [7]),
            ("batch-corrupt-image", [base64.b64encode(
                b"not an image").decode("ascii")]),
        ]
        for name, values in batch_cases:
            invalid.append(expect_error(name, 400, "invalid_request",
                post_json(recognize, {"images_base64": values}, secret)))

        invalid.append(expect_error("batch-too-large", 413,
            "batch_limit_exceeded", post_json(recognize,
                {"images_base64": ["AAAA"] *
                    (config["max_batch_images"] + 1)}, secret)))
        cumulative_image = base64.b64encode(
            make_solid_png(800, 700)).decode("ascii")
        invalid.append(expect_error("batch-decoded-limit", 413,
            "batch_limit_exceeded", post_json(recognize,
                {"images_base64": [cumulative_image, cumulative_image]},
                secret)))

        invalid.append(expect_error("oversized-request", 413,
            "payload_too_large", request(ocr,
                b"x" * (config["max_request_bytes"] + 1),
                "application/octet-stream", secret)))
        invalid.append(expect_error("pixel-limit", 400, "invalid_request",
            request(ocr, make_solid_png(1200, 1000), "image/png", secret)))

        sample = (package / "models/ppocrv6-tiny/sample.jpg").read_bytes()
        status, headers, recovered = request(
            ocr, sample, "image/jpeg", secret, timeout=120)
        assert status == 200 and recovered.get("ok") is True
        assert recovered.get("result")
        assert headers.get("X-Request-ID") == recovered.get("request_id")
        assert process.poll() is None
        print(json.dumps({
            "invalid_cases": len(invalid),
            "names": invalid,
            "post_error_recovery": "passed",
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
