#!/usr/bin/env python3
"""Run deterministic OCR correctness cases against a packaged HTTP service."""

import argparse
import base64
import hashlib
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


def configure_utf8_output() -> None:
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            reconfigure(encoding="utf-8", errors="backslashreplace")


def request_json(url: str, data=None, content_type=None, timeout=120):
    request = urllib.request.Request(url, data=data)
    if content_type:
        request.add_header("Content-Type", content_type)
    with urllib.request.urlopen(request, timeout=timeout) as response:
        headers = dict(response.headers.items())
        return response.status, headers, json.loads(
            response.read().decode("utf-8"))


def make_solid_bmp(width: int, height: int, bgr) -> bytes:
    if width < 1 or height < 1 or len(bgr) != 3:
        raise ValueError("invalid generated BMP dimensions or color")
    row_size = (width * 3 + 3) & ~3
    pixel = bytes(int(value) for value in bgr)
    row = pixel * width + bytes(row_size - width * 3)
    body_size = row_size * height
    file_header = struct.pack("<2sIHHI", b"BM", 54 + body_size, 0, 0, 54)
    info_header = struct.pack(
        "<IIIHHIIIIII", 40, width, height, 1, 24, 0, body_size,
        2835, 2835, 0, 0)
    return file_header + info_header + row * height


def load_image(specification, package):
    if isinstance(specification, str):
        specification = {"path": specification}
    if not isinstance(specification, dict):
        raise AssertionError("image specification must be a path or object")
    if "path" in specification:
        path = package / specification["path"]
        image = path.read_bytes()
        expected_digest = specification.get("sha256")
        if expected_digest:
            actual_digest = hashlib.sha256(image).hexdigest()
            if actual_digest.lower() != expected_digest.lower():
                raise AssertionError(
                    f"{path}: image SHA-256 changed: {actual_digest}")
        suffixes = {
            ".jpg": "image/jpeg", ".jpeg": "image/jpeg",
            ".png": "image/png", ".bmp": "image/bmp",
        }
        content_type = specification.get(
            "content_type", suffixes.get(path.suffix.lower(),
                "application/octet-stream"))
        return image, content_type
    generated = specification.get("generated")
    if generated == "solid_bmp":
        return make_solid_bmp(
            int(specification["width"]), int(specification["height"]),
            specification.get("bgr", [245, 245, 245])), "image/bmp"
    raise AssertionError("unsupported image specification")


def bounds(item):
    keys = [f"{axis}{index}" for axis in ("x", "y")
        for index in range(1, 5)]
    if not all(isinstance(item.get(key), (int, float)) for key in keys):
        raise AssertionError("OCR region must contain x1/y1 through x4/y4")
    if "box" in item:
        raise AssertionError("OCR region must not contain a duplicate box field")
    x_values = [float(item[f"x{index}"]) for index in range(1, 5)]
    y_values = [float(item[f"y{index}"]) for index in range(1, 5)]
    return [min(x_values), min(y_values), max(x_values), max(y_values)]


def validate_timing(document, operation):
    timing = document.get("timing")
    if not isinstance(timing, dict) or not isinstance(
            timing.get("decode_ms"), (int, float)):
        raise AssertionError(f"{operation}: invalid timing object")
    required = ["classifier", "recognizer", "pipeline"]
    if operation == "ocr":
        required.insert(0, "detector")
    for name in required:
        stage = timing.get(name)
        if not isinstance(stage, dict):
            raise AssertionError(f"{operation}: missing {name} timing")
        for field in ("preprocess_ms", "inference_ms", "postprocess_ms",
                      "total_ms"):
            value = stage.get(field)
            if not isinstance(value, (int, float)) or value < 0:
                raise AssertionError(
                    f"{operation}: invalid {name}.{field} timing")
    server_total = timing.get("server_total_ms")
    if not isinstance(server_total, (int, float)) or server_total < 0:
        raise AssertionError(f"{operation}: invalid server_total_ms")


def send_case(case, package, base_url):
    operation = case["operation"]
    endpoint = "/api/ocr" if operation == "ocr" else "/api/recognize"
    if operation == "recognize_batch":
        images = [load_image(specification, package)[0]
            for specification in case["images"]]
        body = json.dumps({"images_base64": [
            base64.b64encode(image).decode("ascii") for image in images
        ]}).encode("utf-8")
        return request_json(base_url + endpoint, body, "application/json")

    image, content_type = load_image(case["image"], package)
    transport = case.get("transport", "binary")
    if transport == "binary":
        return request_json(base_url + endpoint, image, content_type)
    if transport == "json_base64":
        encoded = base64.b64encode(image).decode("ascii")
        body = json.dumps({"image_base64": encoded}).encode("utf-8")
        return request_json(base_url + endpoint, body, "application/json")
    raise AssertionError(f"unsupported transport: {transport}")


def validate_item(case, index, actual, expected, require_bounds):
    if actual.get("text") != expected["text"]:
        raise AssertionError(
            f"{case['id']} item {index}: expected text "
            f"{expected['text']!r}, got {actual.get('text')!r}")
    if actual.get("cls_label") != expected["cls_label"]:
        raise AssertionError(
            f"{case['id']} item {index}: classifier label changed")
    minimum_score = float(expected.get(
        "minimum_score", case.get("minimum_score", 0.0)))
    if float(actual.get("score", -1)) < minimum_score:
        raise AssertionError(
            f"{case['id']} item {index}: score below {minimum_score}")
    minimum_cls_score = float(expected.get(
        "minimum_cls_score", case.get("minimum_cls_score", 0.0)))
    if float(actual.get("cls_score", -1)) < minimum_cls_score:
        raise AssertionError(
            f"{case['id']} item {index}: cls_score below "
            f"{minimum_cls_score}")

    if not require_bounds:
        if actual.get("source_index") != index:
            raise AssertionError(
                f"{case['id']} item {index}: source_index changed")
        for coordinate in [f"{axis}{point}" for axis in ("x", "y")
                           for point in range(1, 5)]:
            if coordinate in actual:
                raise AssertionError(
                    f"{case['id']} item {index}: recognition result contains "
                    f"unexpected coordinate {coordinate}")
        return 0.0

    actual_bounds = bounds(actual)
    tolerance = float(case["box_tolerance_px"])
    deltas = [abs(actual_value - expected_value)
        for actual_value, expected_value in zip(
            actual_bounds, expected["bounds"])]
    if max(deltas) > tolerance:
        raise AssertionError(
            f"{case['id']} item {index}: bounds changed by "
            f"{max(deltas):.2f}px (limit {tolerance:.2f}px)")
    return max(deltas)


def validate_case(case, package, base_url):
    status, headers, document = send_case(case, package, base_url)
    if status != 200 or document.get("ok") is not True:
        raise AssertionError(f"{case['id']}: inference failed: {document}")
    if document.get("backend") != "opencv-dnn":
        raise AssertionError(f"{case['id']}: unexpected backend")
    if headers.get("X-Request-ID") != document.get("request_id"):
        raise AssertionError(f"{case['id']}: request ID header mismatch")

    operation = case["operation"]
    validate_timing(document, operation)
    if operation == "ocr":
        expected_width, expected_height = case["image_size"]
        if document.get("image") != {
                "width": expected_width, "height": expected_height}:
            raise AssertionError(
                f"{case['id']}: unexpected image dimensions: "
                f"{document.get('image')}")
    elif operation == "recognize_batch":
        if document.get("image_count") != len(case["images"]):
            raise AssertionError(f"{case['id']}: image_count changed")

    actual_items = document.get("result")
    expected_items = case["expected"]
    if not isinstance(actual_items, list) or \
            len(actual_items) != len(expected_items):
        raise AssertionError(
            f"{case['id']}: expected {len(expected_items)} items, "
            f"got {len(actual_items) if isinstance(actual_items, list) else 'invalid'}")

    maximum_box_delta = 0.0
    for index, (actual, expected) in enumerate(
            zip(actual_items, expected_items)):
        maximum_box_delta = max(maximum_box_delta, validate_item(
            case, index, actual, expected, operation == "ocr"))
    scores = [float(item["score"]) for item in actual_items]
    return {
        "id": case["id"],
        "operation": operation,
        "items": len(actual_items),
        "maximum_box_delta_px": round(maximum_box_delta, 3),
        "minimum_actual_score": round(min(scores), 6) if scores else None,
    }


def main() -> int:
    configure_utf8_output()
    parser = argparse.ArgumentParser()
    parser.add_argument("--package-dir", type=pathlib.Path, required=True)
    parser.add_argument(
        "--cases", type=pathlib.Path,
        default=pathlib.Path("tests/regression/ppocrv6-tiny.json"))
    parser.add_argument("--port", type=int, default=18786)
    args = parser.parse_args()

    package = args.package_dir.resolve()
    cases_document = json.loads(
        args.cases.resolve().read_text(encoding="utf-8"))
    if cases_document.get("schema_version") != 2 or \
            not cases_document.get("cases"):
        raise RuntimeError("invalid OCR regression case file")
    executable = package / ("lw-ppocr-http-service.exe"
        if platform.system() == "Windows" else "lw-ppocr-http-service")
    config = package / "http-service.json"
    environment = os.environ.copy()
    environment.update({
        "LW_PPOCR_LISTEN_HOST": "127.0.0.1",
        "LW_PPOCR_PORT": str(args.port),
        "LW_PPOCR_API_KEY": "",
        "LW_PPOCR_WORKER_THREADS": "2",
        "LW_PPOCR_FILE_LOGGING_ENABLED": "false",
        "LW_PPOCR_ACCESS_FILE_LOGGING_ENABLED": "false",
        "LW_PPOCR_REQUEST_LOGGING_ENABLED": "false",
    })

    process = subprocess.Popen(
        [str(executable), "--config", str(config)], cwd=package,
        env=environment, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, encoding="utf-8", errors="replace")
    output = ""
    try:
        base_url = f"http://127.0.0.1:{args.port}"
        for _ in range(160):
            if process.poll() is not None:
                break
            try:
                status, _, health = request_json(
                    base_url + "/health", timeout=2)
                if status == 200 and health.get("ok") is True:
                    break
            except (urllib.error.URLError, TimeoutError):
                pass
            time.sleep(0.25)
        else:
            raise RuntimeError("OCR service readiness timed out")
        if process.poll() is not None:
            raise RuntimeError("OCR service exited before readiness")

        started = time.perf_counter()
        results = [validate_case(case, package, base_url)
            for case in cases_document["cases"]]
        elapsed_ms = (time.perf_counter() - started) * 1000.0
        if process.poll() is not None:
            raise RuntimeError("OCR service exited during regression tests")
        print(json.dumps({
            "model": cases_document.get("model"),
            "cases": results,
            "elapsed_ms": round(elapsed_ms, 2),
            "status": "passed",
        }, ensure_ascii=False, indent=2))
        return 0
    finally:
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


if __name__ == "__main__":
    raise SystemExit(main())
