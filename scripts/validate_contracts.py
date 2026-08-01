#!/usr/bin/env python3
"""Validate frozen HTTP/config/log schemas and their immutable digest lock."""

import argparse
import hashlib
import json
import pathlib
import sys

from json_schema import validate


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=pathlib.Path,
        default=pathlib.Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    root = args.root.resolve()
    schema_directory = root / "schemas"
    lock_path = schema_directory / "contracts-v1.lock.json"
    lock = json.loads(lock_path.read_text(encoding="utf-8"))
    assert lock.get("lock_version") == 1
    expected_files = {
        "http-api-v1.openapi.json",
        "http-service-config-v1.schema.json",
        "access-log-v1.schema.json",
    }
    assert set(lock.get("files", {})) == expected_files
    for name in sorted(expected_files):
        path = schema_directory / name
        actual = digest(path)
        expected = lock["files"][name]
        if actual != expected:
            raise RuntimeError(
                f"frozen contract changed: {name}: expected {expected}, "
                f"got {actual}; add a v2 schema instead of editing v1")

    config_schema = json.loads((schema_directory /
        "http-service-config-v1.schema.json").read_text(encoding="utf-8"))
    for relative in ("http-service.json", "config/http-service.json"):
        document = json.loads((root / relative).read_text(encoding="utf-8"))
        validate(document, config_schema)

    openapi = json.loads((schema_directory /
        "http-api-v1.openapi.json").read_text(encoding="utf-8"))
    assert openapi.get("openapi") == "3.1.0"
    assert openapi.get("x-contract-version") == 1
    assert set(openapi.get("paths", {})) == {
        "/health", "/api/ocr", "/api/recognize"}
    assert openapi["components"]["headers"]["ApiVersion"]["schema"] == {
        "const": "1"}

    access_schema = json.loads((schema_directory /
        "access-log-v1.schema.json").read_text(encoding="utf-8"))
    sample_access = {
        "log_schema_version": 1,
        "timestamp": "2026-08-01T06:23:35.485Z",
        "level": "info",
        "event": "request_complete",
        "request_id": "example-1",
        "remote_ip": "127.0.0.1",
        "peer_ip": "127.0.0.1",
        "method": "POST",
        "path": "/api/ocr",
        "operation": "ocr",
        "content_type": "image/jpeg",
        "request_format": "binary",
        "status": 200,
        "request_bytes": 1024,
        "response_bytes": 2048,
        "result_count": 1,
        "duration_ms": 12.5,
        "image": {"width": 100, "height": 50},
        "timing_ms": {"server_total": 12.5, "decode": 0.5,
                      "detector": 5.0, "classifier": 1.0,
                      "recognizer": 4.0, "pipeline": 10.0},
    }
    validate(sample_access, access_schema)
    print(json.dumps({
        "contract_lock_version": 1,
        "schemas": sorted(expected_files),
        "config_examples": 2,
        "status": "passed",
    }, indent=2))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"Contract validation failed: {error}", file=sys.stderr)
        raise SystemExit(1)
