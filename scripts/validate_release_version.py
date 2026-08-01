#!/usr/bin/env python3
"""Verify that all release-facing version declarations agree."""

import json
import pathlib
import re


ROOT = pathlib.Path(__file__).resolve().parents[1]
VERSION_PATTERN = re.compile(
    r"^[0-9]+\.[0-9]+\.[0-9]+(?:-[0-9A-Za-z]+(?:\.[0-9A-Za-z]+)*)?$")


def read(relative):
    return (ROOT / relative).read_text(encoding="utf-8")


def require_contains(relative, expected, failures):
    if expected not in read(relative):
        failures.append("{} does not contain {!r}".format(relative, expected))


def main():
    version = read("RELEASE_VERSION").strip()
    if not VERSION_PATTERN.fullmatch(version):
        raise RuntimeError("invalid RELEASE_VERSION: {!r}".format(version))
    numeric = version.split("-", 1)[0]
    major, minor, patch = numeric.split(".")
    failures = []

    require_contains("CMakeLists.txt",
        "project(lw.PPOCR.OpenCVDNN VERSION {} LANGUAGES C CXX)".format(numeric),
        failures)
    require_contains("CMakeLists.txt",
        "docs/releases/v{}.md".format(version), failures)
    require_contains("include/lw/ppocr.h",
        "#define LW_PPOCR_VERSION_MAJOR {}u".format(major), failures)
    require_contains("include/lw/ppocr.h",
        "#define LW_PPOCR_VERSION_MINOR {}u".format(minor), failures)
    require_contains("include/lw/ppocr.h",
        "#define LW_PPOCR_VERSION_PATCH {}u".format(patch), failures)
    require_contains("include/lw/ppocr.h",
        '#define LW_PPOCR_VERSION_STRING "{}"'.format(version), failures)

    for relative in (
            ".github/workflows/windows-x64.yml",
            ".github/workflows/linux-x64.yml",
            ".github/workflows/linux-arm64-uos20.yml",
            ".github/workflows/macos-arm64.yml",
            ".github/workflows/docker-linux-amd64.yml",
            ".github/workflows/security.yml"):
        require_contains(relative, "PACKAGE_VERSION: {}".format(version), failures)

    require_contains("Dockerfile", "ARG VERSION={}".format(version), failures)
    require_contains("docker-compose.yml",
        "ghcr.io/lxw112190/lw.ppocr.opencvdnn:{}".format(version), failures)
    require_contains(".env.example",
        "PPOCR_IMAGE=ghcr.io/lxw112190/lw.ppocr.opencvdnn:{}".format(version),
        failures)

    for relative in ("README.md", "README.en.md", "docs/DOCKER.md",
                     "docs/LINUX-ARM64-UOS20.md"):
        require_contains(relative, version, failures)

    release_note = ROOT / "docs" / "releases" / "v{}.md".format(version)
    if not release_note.is_file():
        failures.append("missing release note: {}".format(
            release_note.relative_to(ROOT).as_posix()))

    sbom_path = ROOT / "sbom" / "lw.PPOCR.OpenCVDNN.cdx.json"
    try:
        sbom = json.loads(sbom_path.read_text(encoding="utf-8"))
        component = sbom["metadata"]["component"]
        if component.get("version") != version:
            failures.append("SBOM component version is {!r}, expected {!r}".format(
                component.get("version"), version))
        if "@{}".format(version) not in component.get("purl", ""):
            failures.append("SBOM component purl does not contain release version")
    except (OSError, ValueError, KeyError, TypeError) as error:
        failures.append("cannot validate SBOM version: {}".format(error))

    if failures:
        raise RuntimeError("release version validation failed:\n- " +
                           "\n- ".join(failures))
    print(json.dumps({
        "release_version": version,
        "numeric_version": numeric,
        "status": "passed",
    }, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
