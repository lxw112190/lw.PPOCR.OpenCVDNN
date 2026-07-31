#!/usr/bin/env bash
set -euo pipefail
source "$(cd "$(dirname "$0")" && pwd)/service-common.sh"
require_root

[[ -x "$ROOT/run-http-service.sh" ]] || {
  echo "run-http-service.sh was not found beside this script." >&2
  exit 1
}
[[ -f "$ROOT/http-service.json" ]] || {
  echo "http-service.json was not found beside this script." >&2
  exit 1
}

service_user="${LW_PPOCR_SERVICE_USER:-${SUDO_USER:-root}}"
id "$service_user" >/dev/null 2>&1 || {
  echo "Service user does not exist: $service_user" >&2
  exit 1
}
service_group="$(id -gn "$service_user")"
mkdir -p "$ROOT/logs"
chown "$service_user:$service_group" "$ROOT/logs"

escape_systemd_value() {
  local value="$1"
  value="${value//\\/\\\\}"
  value="${value//\"/\\\"}"
  value="${value//%/%%}"
  printf '%s' "$value"
}
escaped_root="$(escape_systemd_value "$ROOT")"

cat > "$UNIT_PATH" <<EOF
[Unit]
Description=lw.PPOCR OpenCV DNN HTTP Service
After=network.target

[Service]
Type=simple
User=$service_user
Group=$service_group
WorkingDirectory="$escaped_root"
ExecStart="$escaped_root/run-http-service.sh"
Restart=on-failure
RestartSec=5
TimeoutStartSec=120
TimeoutStopSec=30
KillSignal=SIGTERM
NoNewPrivileges=true
PrivateTmp=true
LimitNOFILE=65536

[Install]
WantedBy=multi-user.target
EOF

chmod 0644 "$UNIT_PATH"
systemctl daemon-reload
systemctl enable --now "$SERVICE_NAME"
systemctl --no-pager --full status "$SERVICE_NAME" || true
echo "Service installed and started: $SERVICE_NAME"
