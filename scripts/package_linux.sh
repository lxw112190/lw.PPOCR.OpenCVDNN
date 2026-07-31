#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 4 ]]; then
  echo "Usage: $0 <build-dir> <opencv-prefix> <output-dir> <version>" >&2
  exit 2
fi

build_dir="$(realpath "$1")"
opencv_prefix="$(realpath "$2")"
output_dir="$(mkdir -p "$3" && realpath "$3")"
version="$4"
package_name="lw.PPOCR.OpenCVDNN-v${version}-linux-x64"
package_dir="${output_dir}/${package_name}"

if [[ -e "$package_dir" ]]; then
  echo "Package staging path already exists: $package_dir" >&2
  exit 2
fi

cmake --install "$build_dir" --prefix "$package_dir"
find "$opencv_prefix/lib" -maxdepth 1 \
  \( -type f -o -type l \) -name 'libopencv*.so*' \
  -exec cp -P '{}' "$package_dir/" \;

# Keep the package runnable on minimal installations without requiring a
# matching host libstdc++ package. glibc and the ELF loader deliberately remain
# system dependencies because bundling them reduces, rather than improves,
# Linux compatibility.
cxx_runtime="$(g++ -print-file-name=libstdc++.so.6)"
gcc_runtime="$(gcc -print-file-name=libgcc_s.so.1)"
[[ -f "$cxx_runtime" ]] || { echo "libstdc++.so.6 not found" >&2; exit 1; }
[[ -f "$gcc_runtime" ]] || { echo "libgcc_s.so.1 not found" >&2; exit 1; }
cp -L "$cxx_runtime" "$package_dir/libstdc++.so.6"
cp -L "$gcc_runtime" "$package_dir/libgcc_s.so.1"
cat > "$package_dir/licenses/GCC-RUNTIME-NOTICE.txt" <<'EOF'
The Linux package redistributes libstdc++.so.6 and libgcc_s.so.1 from GCC.
They are covered by GPL-3.0-or-later with the GCC Runtime Library Exception 3.1.
License information: https://gcc.gnu.org/onlinedocs/libstdc++/manual/license.html
Runtime exception: https://www.gnu.org/licenses/gcc-exception-3.1.html
EOF

cat > "$package_dir/run-http-service.sh" <<'EOF'
#!/usr/bin/env bash
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
export LD_LIBRARY_PATH="$ROOT${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$ROOT/lw-ppocr-http-service" --config "$ROOT/http-service.json" "$@"
EOF
chmod +x "$package_dir/run-http-service.sh" "$package_dir/lw-ppocr-http-service"
find "$package_dir" -type d \
  \( -name bin -o -name obj -o -name __pycache__ \) \
  -prune -exec rm -rf '{}' +

for binary in "$package_dir/lw-ppocr-http-service" \
              "$package_dir/lw-ppocr-c-example" \
              "$package_dir/liblw.PPOCR.OpenCVDNN.so"; do
  if LD_LIBRARY_PATH="$package_dir" ldd "$binary" | grep -q 'not found'; then
    echo "Unresolved shared-library dependency in $binary" >&2
    LD_LIBRARY_PATH="$package_dir" ldd "$binary" >&2
    exit 1
  fi
done

tar -C "$output_dir" -czf "${output_dir}/${package_name}.tar.gz" "$package_name"
(cd "$output_dir" && \
  sha256sum "${package_name}.tar.gz" > "${package_name}.tar.gz.sha256")
echo "Created ${output_dir}/${package_name}.tar.gz"
