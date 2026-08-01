#!/usr/bin/env python3
"""Verify vendored dependency versions, licenses, and source-tree hashes."""

import argparse
import json
import pathlib
import re
import sys

from dependency_tools import component_tree_digest


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=pathlib.Path,
        default=pathlib.Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    root = args.root.resolve()
    lock = json.loads((root / "dependencies.lock.json").read_text(
        encoding="utf-8"))
    if lock.get("lock_version") != 1:
        raise RuntimeError("unsupported dependency lock version")
    if lock.get("release_opencv_version") != "5.0.0":
        raise RuntimeError("release OpenCV version must remain 5.0.0")

    names = set()
    references = set()
    checked_files = 0
    for component in lock.get("components", []):
        name = component["name"]
        reference = component["bom_ref"]
        if name in names or reference in references:
            raise RuntimeError(f"duplicate dependency identity: {name}")
        names.add(name)
        references.add(reference)
        license_path = root / component["license_file"]
        if not license_path.is_file() or license_path.stat().st_size == 0:
            raise RuntimeError(f"missing dependency license: {license_path}")
        for marker in component.get("version_markers", []):
            text = (root / marker["file"]).read_text(
                encoding="utf-8", errors="replace")
            if marker["contains"] not in text:
                raise RuntimeError(
                    f"version marker changed for {name}: {marker['contains']}")
        paths = component.get("paths", [])
        if paths:
            actual, records = component_tree_digest(root, paths)
            checked_files += len(records)
            expected = component.get("tree_sha256")
            if actual != expected:
                raise RuntimeError(
                    f"dependency tree changed for {name}: expected "
                    f"{expected}, got {actual}")
        elif component.get("tree_sha256") is not None:
            raise RuntimeError(
                f"external dependency {name} must use a null tree hash")
        if not paths:
            source_commit = component.get("source_commit", "")
            if re.fullmatch(r"[0-9a-f]{40}", source_commit) is None:
                raise RuntimeError(
                    f"external dependency {name} requires a source commit")
            source_digest = component.get("source_archive_sha256", "")
            if re.fullmatch(r"[0-9a-f]{64}", source_digest) is None:
                raise RuntimeError(
                    f"external dependency {name} requires a source archive SHA-256")
        for field in ("source_archive_sha256", "windows_archive_sha256"):
            if field in component and re.fullmatch(
                    r"[0-9a-f]{64}", component[field]) is None:
                raise RuntimeError(f"invalid {field} for {name}")

    notice = (root / "THIRD-PARTY-NOTICES.md").read_text(encoding="utf-8")
    notice_names = {
        "nlohmann-json": "nlohmann/json",
        "PP-OCRv6 Tiny Chinese models": "PaddleOCR models",
    }
    for component in lock["components"]:
        marker = notice_names.get(component["name"], component["name"])
        if marker not in notice:
            raise RuntimeError(
                f"THIRD-PARTY-NOTICES.md does not mention {component['name']}")
    print(json.dumps({
        "lock_version": 1,
        "components": len(lock["components"]),
        "vendored_files_checked": checked_files,
        "release_opencv_version": lock["release_opencv_version"],
        "status": "passed",
    }, indent=2))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"Dependency validation failed: {error}", file=sys.stderr)
        raise SystemExit(1)
