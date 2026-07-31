#!/usr/bin/env bash
set -euo pipefail

SERVICE_NAME="lw-ppocr-opencvdnn.service"
UNIT_PATH="/etc/systemd/system/${SERVICE_NAME}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

require_root() {
  if [[ "$(id -u)" -ne 0 ]]; then
    echo "Please run this script with sudo or as root." >&2
    exit 1
  fi
  command -v systemctl >/dev/null 2>&1 || {
    echo "systemctl was not found; this package requires systemd." >&2
    exit 1
  }
}
