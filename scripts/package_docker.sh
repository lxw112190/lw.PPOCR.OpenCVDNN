#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "Usage: $0 <output-dir> <version>" >&2
  exit 2
fi

root="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$1"
output_dir="$(cd "$1" && pwd)"
version="$2"
package_name="lw.PPOCR.OpenCVDNN-v${version}-docker-linux-amd64"
package_dir="${output_dir}/${package_name}"

if [[ -e "$package_dir" ]]; then
  echo "Package staging path already exists: $package_dir" >&2
  exit 2
fi

release_note="$root/docs/releases/v${version}.md"
if [[ ! -f "$release_note" ]]; then
  echo "Release note is missing: $release_note" >&2
  exit 2
fi

mkdir -p "$package_dir/docs/releases"
cp "$root/docker-compose.yml" "$package_dir/"
cp "$root/.env.example" "$package_dir/"
cp "$root/RELEASE_VERSION" "$package_dir/"
cp "$root/docs/DOCKER.md" "$package_dir/docs/"
cp "$release_note" "$package_dir/docs/releases/"
cp "$root/LICENSE" "$package_dir/"

tar -C "$output_dir" -czf "${output_dir}/${package_name}.tar.gz" \
  "$package_name"
(cd "$output_dir" && \
  sha256sum "${package_name}.tar.gz" > "${package_name}.tar.gz.sha256")
echo "Created ${output_dir}/${package_name}.tar.gz"
