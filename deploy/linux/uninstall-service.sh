#!/usr/bin/env bash
set -euo pipefail
source "$(cd "$(dirname "$0")" && pwd)/service-common.sh"
require_root

if [[ ! -f "$UNIT_PATH" ]]; then
  echo "Service is not installed: $SERVICE_NAME"
  exit 0
fi
systemctl disable --now "$SERVICE_NAME" || true
rm -f "$UNIT_PATH"
systemctl daemon-reload
systemctl reset-failed "$SERVICE_NAME" 2>/dev/null || true
echo "Service uninstalled: $SERVICE_NAME"
