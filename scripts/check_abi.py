#!/usr/bin/env python3
"""Check that a native library still exports every frozen C ABI v1 symbol."""

import argparse
import ctypes
import json
import os
import pathlib
import sys


def configure_utf8_output() -> None:
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            reconfigure(encoding="utf-8", errors="backslashreplace")


def main() -> int:
    configure_utf8_output()
    parser = argparse.ArgumentParser()
    parser.add_argument("--library", type=pathlib.Path, required=True)
    parser.add_argument(
        "--baseline", type=pathlib.Path,
        default=pathlib.Path("tests/abi/exports-v1.txt"))
    args = parser.parse_args()

    library = args.library.resolve()
    baseline = args.baseline.resolve()
    if not library.is_file():
        raise RuntimeError(f"native library does not exist: {library}")
    if not baseline.is_file():
        raise RuntimeError(f"ABI baseline does not exist: {baseline}")

    symbols = [line.strip() for line in baseline.read_text(
        encoding="utf-8").splitlines()
        if line.strip() and not line.lstrip().startswith("#")]
    if not symbols or len(symbols) != len(set(symbols)):
        raise RuntimeError("ABI baseline is empty or contains duplicate symbols")

    dll_directory = None
    if os.name == "nt" and hasattr(os, "add_dll_directory"):
        dll_directory = os.add_dll_directory(str(library.parent))
    try:
        native = ctypes.CDLL(str(library))
        missing = [name for name in symbols if not hasattr(native, name)]
    finally:
        if dll_directory is not None:
            dll_directory.close()

    if missing:
        raise RuntimeError("missing frozen C ABI exports: " + ", ".join(missing))
    print(json.dumps({
        "api_version": 1,
        "library": str(library),
        "verified_exports": len(symbols),
        "status": "passed",
    }, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
