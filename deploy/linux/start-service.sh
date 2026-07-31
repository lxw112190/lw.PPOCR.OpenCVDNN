#!/usr/bin/env bash
set -euo pipefail
source "$(cd "$(dirname "$0")" && pwd)/service-common.sh"
require_root
systemctl start "$SERVICE_NAME"
systemctl --no-pager --full status "$SERVICE_NAME"
