#!/usr/bin/env bash
set -euo pipefail
source "$(cd "$(dirname "$0")" && pwd)/service-common.sh"
require_root
systemctl stop "$SERVICE_NAME"
echo "Service stopped: $SERVICE_NAME"
