#!/usr/bin/env python3
"""Start a packaged HTTP service and verify health/OCR/recognition."""

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


def configure_utf8_output() -> None:
    """Keep CI logs printable when OCR or service output contains Unicode."""
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            reconfigure(encoding="utf-8", errors="backslashreplace")


def request_data(url: str, data=None, content_type=None):
    request = urllib.request.Request(url, data=data)
    if content_type:
        request.add_header("Content-Type", content_type)
    with urllib.request.urlopen(request, timeout=60) as response:
        return json.loads(response.read().decode("utf-8"))


def request_json(url: str, body=None):
    data = None if body is None else json.dumps(body).encode("utf-8")
    return request_data(url, data, "application/json" if data else None)


def request_binary(url: str, image: bytes, content_type="application/octet-stream"):
    return request_data(url, image, content_type)


def error_status(function) -> int:
    try:
        function()
    except urllib.error.HTTPError as error:
        return error.code
    raise AssertionError("request unexpectedly succeeded")


def main() -> int:
    configure_utf8_output()
    parser = argparse.ArgumentParser()
    parser.add_argument("--package-dir", type=pathlib.Path, required=True)
    parser.add_argument("--port", type=int, default=8787)
    args = parser.parse_args()
    package = args.package_dir.resolve()
    binary = package / ("lw-ppocr-http-service.exe"
        if platform.system() == "Windows" else "lw-ppocr-http-service")
    source_config = json.loads(
        (package / "http-service.json").read_text(encoding="utf-8"))
    source_config["port"] = args.port + 1 if args.port < 65535 else args.port - 1
    config_path = package / f".http-smoke-{os.getpid()}.json"
    config_path.write_text(
        json.dumps(source_config, ensure_ascii=False, indent=2), encoding="utf-8")
    process_environment = os.environ.copy()
    process_environment.update({
        "LW_PPOCR_LISTEN_HOST": "127.0.0.1",
        "LW_PPOCR_PORT": str(args.port),
        "LW_PPOCR_API_KEY": "",
        "LW_PPOCR_WORKER_THREADS": "2",
        "LW_PPOCR_FILE_LOGGING_ENABLED": "false",
    })
    process = None
    completed = False
    try:
        process = subprocess.Popen(
            [str(binary), "--config", str(config_path)], cwd=package,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
            encoding="utf-8", errors="replace", env=process_environment)
        health = None
        for _ in range(120):
            if process.poll() is not None:
                break
            try:
                health = request_json(f"http://127.0.0.1:{args.port}/health")
                break
            except (urllib.error.URLError, TimeoutError):
                time.sleep(0.25)
        if health is None:
            output = process.stdout.read() if process.stdout else ""
            raise RuntimeError(f"service did not become ready\n{output}")
        with urllib.request.urlopen(
                f"http://127.0.0.1:{args.port}/", timeout=10) as response:
            page = response.read().decode("utf-8")
        with urllib.request.urlopen(
                f"http://127.0.0.1:{args.port}/sponsor.jpg", timeout=10) as response:
            sponsor = response.read()
        assert "lw.PPOCR.OpenCVDNN 在线体验" in page
        assert "819069052" in page and "758616458" not in page
        assert "body: selectedImage" in page
        assert len(sponsor) > 1000
        sample = (package / "models/ppocrv6-tiny/sample.jpg").read_bytes()
        image_base64 = base64.b64encode(sample).decode("ascii")
        ocr = request_json(f"http://127.0.0.1:{args.port}/api/ocr",
            {"image_base64": image_base64})
        binary_ocr = request_binary(
            f"http://127.0.0.1:{args.port}/api/ocr", sample, "image/jpeg")
        recognize = request_json(
            f"http://127.0.0.1:{args.port}/api/recognize",
            {"image_base64": image_base64})
        binary_recognize = request_binary(
            f"http://127.0.0.1:{args.port}/api/recognize", sample, "image/jpeg")
        batch = request_json(
            f"http://127.0.0.1:{args.port}/api/recognize",
            {"images_base64": [image_base64, image_base64]})
        bad_image_status = error_status(lambda: request_binary(
            f"http://127.0.0.1:{args.port}/api/ocr", b"not an image"))
        assert health["ok"] and health["backend"] == "opencv-dnn"
        assert ocr["ok"] and ocr["result"] and ocr["result"][0]["text"]
        assert binary_ocr["ok"] and binary_ocr["result"]
        legacy_box_keys = {
            f"{axis}{index}" for axis in ("x", "y") for index in range(1, 5)
        }
        for item in ocr["result"]:
            assert isinstance(item.get("box"), list) and len(item["box"]) == 4
            assert all(
                isinstance(point, dict) and
                isinstance(point.get("x"), (int, float)) and
                isinstance(point.get("y"), (int, float))
                for point in item["box"])
            assert legacy_box_keys.isdisjoint(item)
        assert recognize["ok"] and recognize["result"]
        assert binary_recognize["ok"] and binary_recognize["result"]
        assert batch["ok"] and len(batch["result"]) == 2
        assert bad_image_status == 400
        print(json.dumps({
            "health": health["status"],
            "web_page": "ok",
            "ocr_regions": len(ocr["result"]),
            "binary_ocr_regions": len(binary_ocr["result"]),
            "first_text": ocr["result"][0]["text"],
            "recognition_items": len(recognize["result"]),
            "binary_recognition_items": len(binary_recognize["result"]),
            "batch_items": len(batch["result"]),
            "invalid_image_status": bad_image_status,
            "server_total_ms": ocr["timing"]["server_total_ms"],
        }, ensure_ascii=False, indent=2))
        completed = True
        return 0
    finally:
        if process is not None:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
            if process.stdout:
                output = process.stdout.read()
                assert "758616458" not in output
                if completed:
                    assert "request_started" in output
                    assert f"port: {args.port}" in output
                    assert "worker_threads: 2" in output
                    assert "logging.file_enabled: false" in output
                print("--- service output ---")
                print(output)
        config_path.unlink(missing_ok=True)


if __name__ == "__main__":
    raise SystemExit(main())
