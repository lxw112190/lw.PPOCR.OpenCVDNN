#!/usr/bin/env python3
"""Verify that the native service enforces configuration Schema v1."""

import argparse
import copy
import json
import os
import pathlib
import platform
import subprocess
import sys


def configure_utf8_output() -> None:
    for stream in (sys.stdout, sys.stderr):
        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is not None:
            reconfigure(encoding="utf-8", errors="backslashreplace")


def main() -> int:
    configure_utf8_output()
    parser = argparse.ArgumentParser()
    parser.add_argument("--package-dir", type=pathlib.Path, required=True)
    args = parser.parse_args()
    package = args.package_dir.resolve()
    executable = package / ("lw-ppocr-http-service.exe"
        if platform.system() == "Windows" else "lw-ppocr-http-service")
    baseline = json.loads((package / "http-service.json").read_text(
        encoding="utf-8"))

    cases = []

    def add(name, mutate, expected):
        value = copy.deepcopy(baseline)
        mutate(value)
        cases.append((name, value, expected))

    add("missing-version", lambda value: value.pop("schema_version"),
        "requires schema_version")
    add("future-version", lambda value: value.update(schema_version=2),
        "unsupported HTTP service configuration schema_version")
    add("unknown-root", lambda value: value.update(unknown_option=True),
        "contains unknown property")
    add("unknown-logging",
        lambda value: value["logging"].update(unknown_option=True),
        "contains unknown property")
    add("empty-web-root", lambda value: value.update(web_root=""),
        "must not be empty")
    add("unknown-log-level",
        lambda value: value["logging"].update(level="verbose"),
        "logging.level is unsupported")
    add("oversized-log-file",
        lambda value: value["logging"].update(max_file_size_mb=1025),
        "logging.max_file_size_mb must be between 1 and 1024")
    add("duplicate-proxy",
        lambda value: value["logging"].update(
            trusted_proxies=["127.0.0.1", "127.0.0.1"]),
        "entries must be unique")
    add("zero-queue-limit",
        lambda value: value.update(max_queued_requests=0),
        "configuration contains an out-of-range value")
    add("zero-engine-timeout",
        lambda value: value.update(engine_wait_timeout_ms=0),
        "configuration contains an out-of-range value")
    add("batch-count-over-limit",
        lambda value: value.update(max_batch_images=257),
        "configuration contains an out-of-range value")
    add("batch-pixels-below-single-image-limit",
        lambda value: value.update(max_batch_total_pixels=1),
        "configuration contains an out-of-range value")

    environment = os.environ.copy()
    environment.update({
        "LW_PPOCR_LOGGING_ENABLED": "false",
        "LW_PPOCR_API_KEY": "",
    })
    passed = []
    for name, document, expected in cases:
        path = package / (".config-contract-{}-{}.json".format(
            os.getpid(), name))
        path.write_text(json.dumps(
            document, ensure_ascii=False, indent=2), encoding="utf-8")
        try:
            result = subprocess.run(
                [str(executable), "--config", str(path)], cwd=package,
                env=environment, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, timeout=20, text=True,
                encoding="utf-8", errors="replace")
        finally:
            try:
                path.unlink()
            except FileNotFoundError:
                pass
        if result.returncode == 0 or expected not in result.stdout:
            raise AssertionError(
                "{}: expected early rejection containing {!r}; exit={}\n{}"
                .format(name, expected, result.returncode, result.stdout))
        passed.append(name)

    print(json.dumps({
        "config_schema_version": 1,
        "rejected_cases": passed,
        "status": "passed",
    }, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
