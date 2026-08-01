#!/usr/bin/env python3
"""Validate frozen model manifest schema v1 and optional file digests."""

import argparse
import hashlib
import json
import pathlib
import re
import sys


HEX_64 = re.compile(r"^[0-9a-fA-F]{64}$")


def require_object(value, context):
    if not isinstance(value, dict):
        raise ValueError(f"{context} must be an object")
    return value


def reject_unknown(value, allowed, context):
    unknown = sorted(set(value) - set(allowed))
    if unknown:
        raise ValueError(f"{context} contains unknown properties: {unknown}")


def require_string(value, name, context):
    result = value.get(name)
    if not isinstance(result, str) or not result:
        raise ValueError(f"{context}.{name} must be a non-empty string")
    return result


def validate_file(value, base, context, require_sha256, artifact=False):
    require_object(value, context)
    allowed = {"path", "sha256"}
    if artifact:
        allowed.update({"format", "precision"})
    reject_unknown(value, allowed, context)
    relative_path = require_string(value, "path", context)
    if artifact:
        if value.get("format") != "onnx":
            raise ValueError(f"{context}.format must be 'onnx'")
        if value.get("precision") not in {"fp32", "fp16", "int8"}:
            raise ValueError(f"{context}.precision is unsupported")
    digest = value.get("sha256")
    if require_sha256 and digest is None:
        raise ValueError(f"{context}.sha256 is required")
    if digest is not None and (not isinstance(digest, str) or
            HEX_64.fullmatch(digest) is None):
        raise ValueError(f"{context}.sha256 must be 64 hexadecimal characters")
    path = pathlib.Path(relative_path)
    if not path.is_absolute():
        path = base / path
    path = path.resolve()
    if not path.is_file():
        raise ValueError(f"{context} file does not exist: {path}")
    if digest is not None:
        actual = hashlib.sha256(path.read_bytes()).hexdigest()
        if actual.lower() != digest.lower():
            raise ValueError(
                f"{context}.sha256 mismatch: expected {digest}, got {actual}")
    return path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=pathlib.Path, required=True)
    parser.add_argument("--require-sha256", action="store_true")
    args = parser.parse_args()

    manifest_path = args.manifest.resolve()
    document = require_object(json.loads(
        manifest_path.read_text(encoding="utf-8")), "model manifest")
    reject_unknown(document, {
        "$schema", "schema_version", "name", "family", "language",
        "dictionary", "stages",
    }, "model manifest")
    if type(document.get("schema_version")) is not int or \
            document["schema_version"] != 1:
        raise ValueError("schema_version must be the frozen integer value 1")
    require_string(document, "name", "model manifest")
    require_string(document, "family", "model manifest")
    if "$schema" in document:
        require_string(document, "$schema", "model manifest")
    if "language" in document:
        require_string(document, "language", "model manifest")

    base = manifest_path.parent
    validate_file(document.get("dictionary"), base, "dictionary",
        args.require_sha256)
    stages = require_object(document.get("stages"), "stages")
    reject_unknown(stages, {"detector", "classifier", "recognizer"}, "stages")
    for required in ("detector", "recognizer"):
        if required not in stages:
            raise ValueError(f"stages.{required} is required")
    files = 1
    for stage_name, stage in stages.items():
        context = f"stages.{stage_name}"
        require_object(stage, context)
        reject_unknown(stage, {
            "input_name", "output_name", "input_shape", "artifacts",
        }, context)
        shape = stage.get("input_shape")
        if not isinstance(shape, list) or not 3 <= len(shape) <= 4 or \
                any(type(value) is not int for value in shape):
            raise ValueError(f"{context}.input_shape must contain 3 or 4 integers")
        for optional in ("input_name", "output_name"):
            if optional in stage and not isinstance(stage[optional], str):
                raise ValueError(f"{context}.{optional} must be a string")
        artifacts = require_object(stage.get("artifacts"), context + ".artifacts")
        reject_unknown(artifacts, {"onnx"}, context + ".artifacts")
        if "onnx" not in artifacts:
            raise ValueError(f"{context}.artifacts.onnx is required")
        validate_file(artifacts["onnx"], base,
            context + ".artifacts.onnx", args.require_sha256, artifact=True)
        files += 1

    print(json.dumps({
        "manifest": str(manifest_path),
        "schema_version": 1,
        "verified_files": files,
        "sha256_required": args.require_sha256,
        "status": "passed",
    }, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"Model manifest validation failed: {error}", file=sys.stderr)
        raise SystemExit(1)
