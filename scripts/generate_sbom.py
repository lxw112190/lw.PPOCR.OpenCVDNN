#!/usr/bin/env python3
"""Generate a deterministic CycloneDX 1.6 SBOM from dependencies.lock.json."""

import argparse
import json
import pathlib


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=pathlib.Path,
        default=pathlib.Path(__file__).resolve().parents[1])
    parser.add_argument("--version", required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    lock = json.loads((root / "dependencies.lock.json").read_text(
        encoding="utf-8"))
    project_ref = f"pkg:github/lxw112190/lw.PPOCR.OpenCVDNN@{args.version}"
    components = []
    for value in lock["components"]:
        component = {
            "type": value["type"],
            "bom-ref": value["bom_ref"],
            "name": value["name"],
            "version": value["version"],
            "licenses": [{"license": {"id": value["license"]}}],
            "purl": value["purl"],
            "externalReferences": [{
                "type": "vcs", "url": value["source_url"]}],
        }
        component_digest = (value.get("tree_sha256") or
            value.get("source_archive_sha256"))
        if component_digest:
            component["hashes"] = [{
                "alg": "SHA-256", "content": component_digest}]
        archive_properties = []
        for field in ("source_commit", "source_archive_sha256",
                      "windows_archive_sha256"):
            if value.get(field):
                archive_properties.append({
                    "name": "lw.ppocr." + field,
                    "value": value[field],
                })
        if archive_properties:
            component["properties"] = archive_properties
        components.append(component)
    document = {
        "bomFormat": "CycloneDX",
        "specVersion": "1.6",
        "version": 1,
        "metadata": {
            "component": {
                "type": "application",
                "bom-ref": project_ref,
                "name": "lw.PPOCR.OpenCVDNN",
                "version": args.version,
                "purl": project_ref,
            },
            "properties": [{
                "name": "lw.ppocr.release_opencv_version",
                "value": lock["release_opencv_version"],
            }],
        },
        "components": components,
        "dependencies": [{
            "ref": project_ref,
            "dependsOn": [component["bom-ref"] for component in components],
        }] + [{
            "ref": component["bom-ref"],
            "dependsOn": (["pkg:github/fmtlib/fmt@12.1.0"]
                if component["name"] == "spdlog" else []),
        } for component in components],
    }
    output = args.output
    if not output.is_absolute():
        output = root / output
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(
        document, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"Created {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
