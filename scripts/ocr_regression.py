#!/usr/bin/env python3
"""Run deterministic OCR correctness cases against a packaged HTTP service."""

import argparse
import hashlib
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
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            reconfigure(encoding="utf-8", errors="backslashreplace")


def request_json(url: str, data=None, content_type=None, timeout=120):
    request = urllib.request.Request(url, data=data)
    if content_type:
        request.add_header("Content-Type", content_type)
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return response.status, json.loads(response.read().decode("utf-8"))


def bounds(item):
    keys = [f"{axis}{index}" for axis in ("x", "y") for index in range(1, 5)]
    if not all(isinstance(item.get(key), (int, float)) for key in keys):
        raise AssertionError("OCR region must contain x1/y1 through x4/y4")
    if "box" in item:
        raise AssertionError("OCR region must not contain a duplicate box field")
    x_values = [float(item[f"x{index}"]) for index in range(1, 5)]
    y_values = [float(item[f"y{index}"]) for index in range(1, 5)]
    return [min(x_values), min(y_values), max(x_values), max(y_values)]


def validate_case(case, package, base_url):
    image_path = package / case["image"]
    image = image_path.read_bytes()
    actual_digest = hashlib.sha256(image).hexdigest()
    if actual_digest.lower() != case["image_sha256"].lower():
        raise AssertionError(
            f"{case['id']}: image SHA-256 changed: {actual_digest}")

    status, document = request_json(
        base_url + case["endpoint"], image, "image/jpeg")
    if status != 200 or document.get("ok") is not True:
        raise AssertionError(f"{case['id']}: OCR failed: {document}")
    if document.get("backend") != "opencv-dnn":
        raise AssertionError(f"{case['id']}: unexpected backend")
    expected_width, expected_height = case["image_size"]
    if document.get("image") != {
            "width": expected_width, "height": expected_height}:
        raise AssertionError(
            f"{case['id']}: unexpected image dimensions: {document.get('image')}")

    actual_items = document.get("result")
    expected_items = case["expected"]
    if not isinstance(actual_items, list) or \
            len(actual_items) != len(expected_items):
        raise AssertionError(
            f"{case['id']}: expected {len(expected_items)} regions, "
            f"got {len(actual_items) if isinstance(actual_items, list) else 'invalid'}")

    tolerance = float(case["box_tolerance_px"])
    minimum_score = float(case["minimum_score"])
    maximum_box_delta = 0.0
    for index, (actual, expected) in enumerate(
            zip(actual_items, expected_items)):
        if actual.get("text") != expected["text"]:
            raise AssertionError(
                f"{case['id']} region {index}: expected text "
                f"{expected['text']!r}, got {actual.get('text')!r}")
        if actual.get("cls_label") != expected["cls_label"]:
            raise AssertionError(
                f"{case['id']} region {index}: classifier label changed")
        if float(actual.get("score", -1)) < minimum_score:
            raise AssertionError(
                f"{case['id']} region {index}: score below {minimum_score}")
        actual_bounds = bounds(actual)
        deltas = [abs(actual_value - expected_value)
            for actual_value, expected_value in zip(
                actual_bounds, expected["bounds"])]
        maximum_box_delta = max(maximum_box_delta, max(deltas))
        if max(deltas) > tolerance:
            raise AssertionError(
                f"{case['id']} region {index}: bounds changed by "
                f"{max(deltas):.2f}px (limit {tolerance:.2f}px)")

    return {
        "id": case["id"],
        "regions": len(actual_items),
        "maximum_box_delta_px": round(maximum_box_delta, 3),
        "minimum_actual_score": round(min(
            float(item["score"]) for item in actual_items), 6),
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
    cases_document = json.loads(args.cases.resolve().read_text(encoding="utf-8"))
    if cases_document.get("schema_version") != 1 or \
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
                status, health = request_json(base_url + "/health", timeout=2)
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
