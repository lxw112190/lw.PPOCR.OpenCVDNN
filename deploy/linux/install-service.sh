#!/usr/bin/env bash
set -euo pipefail
source "$(cd "$(dirname "$0")" && pwd)/service-common.sh"

verify_only=false
case "${1:-}" in
  "") ;;
  --verify-only) verify_only=true ;;
  *)
    echo "Usage: $0 [--verify-only]" >&2
    exit 2
    ;;
esac

if [[ "$verify_only" != true ]]; then
  require_root
fi

[[ -x "$ROOT/run-http-service.sh" ]] || {
  echo "run-http-service.sh was not found beside this script." >&2
  exit 1
}
[[ -f "$ROOT/http-service.json" ]] || {
  echo "http-service.json was not found beside this script." >&2
  exit 1
}

service_user="${LW_PPOCR_SERVICE_USER:-${SUDO_USER:-$(id -un)}}"
id "$service_user" >/dev/null 2>&1 || {
  echo "Service user does not exist: $service_user" >&2
  exit 1
}
service_group="$(id -gn "$service_user")"
bash_path="$(command -v bash || true)"
[[ "$bash_path" == /* && -x "$bash_path" ]] || {
  echo "An executable bash with an absolute path is required." >&2
  exit 1
}

if [[ "$ROOT" == *$'\n'* || "$ROOT" == *$'\r'* ]]; then
  echo "The package path must not contain newline characters." >&2
  exit 1
fi

escape_systemd_path() {
  local value="$1"
  value="${value//\\/\\\\}"
  value="${value//%/%%}"
  value="${value//\$/\\x24}"
  value="${value//\"/\\x22}"
  value="${value//$'\t'/\\x09}"
  value="${value// /\\x20}"
  printf '%s' "$value"
}
escaped_root="$(escape_systemd_path "$ROOT")"

unit_temp_dir="$(mktemp -d)"
unit_candidate="$unit_temp_dir/$SERVICE_NAME"
cleanup() {
  rm -f "$unit_candidate"
  rmdir "$unit_temp_dir" 2>/dev/null || true
}
trap cleanup EXIT

cat > "$unit_candidate" <<EOF
[Unit]
Description=lw.PPOCR OpenCV DNN HTTP Service
After=network.target

[Service]
Type=simple
User=$service_user
Group=$service_group
WorkingDirectory=$escaped_root
ExecStart=$bash_path "$escaped_root/run-http-service.sh"
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

if command -v systemd-analyze >/dev/null 2>&1; then
  if ! systemd-analyze verify "$unit_candidate"; then
    echo "Generated systemd unit validation failed:" >&2
    sed 's/^/  /' "$unit_candidate" >&2
    exit 1
  fi
elif [[ "$verify_only" == true ]]; then
  echo "systemd-analyze was not found; cannot verify the generated unit." >&2
  exit 1
else
  echo "Warning: systemd-analyze was not found; skipping unit validation." >&2
fi

if [[ "$verify_only" == true ]]; then
  echo "Generated systemd unit is valid:"
  sed 's/^/  /' "$unit_candidate"
  exit 0
fi

mkdir -p "$ROOT/logs"
chown "$service_user:$service_group" "$ROOT/logs"
install -m 0644 "$unit_candidate" "$UNIT_PATH"
systemctl daemon-reload
systemctl enable "$SERVICE_NAME"
if ! systemctl restart "$SERVICE_NAME"; then
  echo "Service failed to start. Generated unit and diagnostics follow:" >&2
  systemctl cat "$SERVICE_NAME" >&2 || true
  systemctl --no-pager --full status "$SERVICE_NAME" >&2 || true
  journalctl --no-pager -u "$SERVICE_NAME" -n 80 >&2 || true
  exit 1
fi
systemctl --no-pager --full status "$SERVICE_NAME" || true
echo "Service installed and started: $SERVICE_NAME"
