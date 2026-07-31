#!/usr/bin/env python3
"""Start a packaged HTTP service and verify health/OCR/recognition."""

import argparse
import base64
import json
import pathlib
import platform
import subprocess
import time
import urllib.error
import urllib.request


def request_json(url: str, body=None):
    data = None if body is None else json.dumps(body).encode("utf-8")
    request = urllib.request.Request(url, data=data)
    if data is not None:
        request.add_header("Content-Type", "application/json")
    with urllib.request.urlopen(request, timeout=60) as response:
        return json.loads(response.read().decode("utf-8"))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--package-dir", type=pathlib.Path, required=True)
    parser.add_argument("--port", type=int, default=8787)
    args = parser.parse_args()
    package = args.package_dir.resolve()
    binary = package / ("lw-ppocr-http-service.exe"
        if platform.system() == "Windows" else "lw-ppocr-http-service")
    process = subprocess.Popen(
        [str(binary)], cwd=package, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace")
    try:
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
        assert len(sponsor) > 1000
        sample = (package / "models/ppocrv6-tiny/sample.jpg").read_bytes()
        image_base64 = base64.b64encode(sample).decode("ascii")
        ocr = request_json(f"http://127.0.0.1:{args.port}/api/ocr",
            {"image_base64": image_base64})
        recognize = request_json(
            f"http://127.0.0.1:{args.port}/api/recognize",
            {"image_base64": image_base64})
        batch = request_json(
            f"http://127.0.0.1:{args.port}/api/recognize",
            {"images_base64": [image_base64, image_base64]})
        bad_image_status = None
        try:
            request_json(f"http://127.0.0.1:{args.port}/api/ocr",
                {"image_base64": base64.b64encode(b"not an image").decode("ascii")})
        except urllib.error.HTTPError as error:
            bad_image_status = error.code
        assert health["ok"] and health["backend"] == "opencv-dnn"
        assert ocr["ok"] and ocr["result"] and ocr["result"][0]["text"]
        assert recognize["ok"] and recognize["result"]
        assert batch["ok"] and len(batch["result"]) == 2
        assert bad_image_status == 400
        print(json.dumps({
            "health": health["status"],
            "web_page": "ok",
            "ocr_regions": len(ocr["result"]),
            "first_text": ocr["result"][0]["text"],
            "recognition_items": len(recognize["result"]),
            "batch_items": len(batch["result"]),
            "invalid_image_status": bad_image_status,
            "server_total_ms": ocr["timing"]["server_total_ms"],
        }, ensure_ascii=False, indent=2))
        return 0
    finally:
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
        if process.stdout:
            output = process.stdout.read()
            assert "758616458" not in output
            assert "request_started" in output
            print("--- service output ---")
            print(output)


if __name__ == "__main__":
    raise SystemExit(main())
