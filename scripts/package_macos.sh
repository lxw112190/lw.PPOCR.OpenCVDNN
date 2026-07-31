#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 4 ]]; then
  echo "Usage: $0 <build-dir> <opencv-prefix> <output-dir> <version>" >&2
  exit 2
fi

build_dir="$(cd "$1" && pwd)"
opencv_prefix="$(cd "$2" && pwd)"
mkdir -p "$3"
output_dir="$(cd "$3" && pwd)"
version="$4"

case "$(uname -m)" in
  arm64) architecture="arm64" ;;
  x86_64) architecture="x64" ;;
  *) echo "Unsupported macOS architecture: $(uname -m)" >&2; exit 2 ;;
esac

package_name="lw.PPOCR.OpenCVDNN-v${version}-macos-${architecture}"
package_dir="${output_dir}/${package_name}"

if [[ -e "$package_dir" ]]; then
  echo "Package staging path already exists: $package_dir" >&2
  exit 2
fi

cmake --install "$build_dir" --prefix "$package_dir"
find "$opencv_prefix/lib" -maxdepth 1 \
  \( -type f -o -type l \) -name 'libopencv*.dylib' \
  -exec cp -P '{}' "$package_dir/" \;

cat > "$package_dir/run-http-service.sh" <<'EOF'
#!/usr/bin/env bash
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
export DYLD_LIBRARY_PATH="$ROOT${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
exec "$ROOT/lw-ppocr-http-service" --config "$ROOT/http-service.json" "$@"
EOF
chmod +x "$package_dir/run-http-service.sh" \
  "$package_dir/lw-ppocr-http-service" "$package_dir/lw-ppocr-c-example"

find "$package_dir" -type d \
  \( -name bin -o -name obj -o -name __pycache__ \) \
  -prune -exec rm -rf '{}' +

for binary in "$package_dir/lw-ppocr-http-service" \
              "$package_dir/lw-ppocr-c-example" \
              "$package_dir/liblw.PPOCR.OpenCVDNN.dylib"; do
  test -f "$binary"
  if otool -L "$binary" | grep -F "$opencv_prefix"; then
    echo "Non-relocatable OpenCV dependency in $binary" >&2
    exit 1
  fi
done

# Ad-hoc signing avoids invalid signatures after packaging while keeping the
# archive independent from a project-specific Apple Developer certificate.
while IFS= read -r -d '' binary; do
  if file "$binary" | grep -q 'Mach-O'; then
    codesign --force --sign - "$binary"
  fi
done < <(find "$package_dir" -type f -print0)

tar -C "$output_dir" -czf "${output_dir}/${package_name}.tar.gz" "$package_name"
shasum -a 256 "${output_dir}/${package_name}.tar.gz" \
  > "${output_dir}/${package_name}.tar.gz.sha256"
echo "Created ${output_dir}/${package_name}.tar.gz"
