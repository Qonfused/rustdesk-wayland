#!/bin/bash
set -euo pipefail

STATE_DIR="${XDG_STATE_HOME:-${HOME}/.local/state}/rustdesk-wayland"
LOG_FILE="${RUSTDESK_SUPERVISOR_LOG:-${STATE_DIR}/supervisor.log}"
LOG_MAX_BYTES="${RUSTDESK_SUPERVISOR_LOG_MAX_BYTES:-1048576}"
LOG_BACKUPS="${RUSTDESK_SUPERVISOR_LOG_BACKUPS:-2}"
LOCK_FILE="${XDG_RUNTIME_DIR:-/tmp}/rustdesk-wayland-${UID}.lock"
START_DELAY="${RUSTDESK_AUTOSTART_DELAY:-5}"
RETRY_DELAY="${RUSTDESK_RETRY_DELAY:-10}"
HEALTH_INTERVAL="${RUSTDESK_HEALTH_INTERVAL:-10}"
WAYLAND_SCRIPT="${HOME}/.local/bin/rustdesk-wayland.sh"
VIRTUAL_DISPLAY="${RUSTDESK_VIRTUAL_DISPLAY:-:98}"
GUI_DISPLAY="${RUSTDESK_GUI_DISPLAY:-:97}"
VIRTUAL_DISPLAY=":${VIRTUAL_DISPLAY#:}"
GUI_DISPLAY=":${GUI_DISPLAY#:}"

mkdir -p "$STATE_DIR"

rotate_log() {
    local size backup
    size=$(stat -c %s "$LOG_FILE" 2>/dev/null || echo 0)
    [ "$size" -lt "$LOG_MAX_BYTES" ] && return 0

    if [ "$LOG_BACKUPS" -gt 0 ]; then
        rm -f "${LOG_FILE}.${LOG_BACKUPS}"
        for ((backup = LOG_BACKUPS - 1; backup >= 1; backup--)); do
            [ ! -f "${LOG_FILE}.${backup}" ] || \
                mv "${LOG_FILE}.${backup}" "${LOG_FILE}.$((backup + 1))"
        done
        mv "$LOG_FILE" "${LOG_FILE}.1"
    else
        rm -f "$LOG_FILE"
    fi
}

log_sink() {
    local line
    while IFS= read -r line; do
        rotate_log
        printf '%s\n' "$line" >> "$LOG_FILE"
    done
}

# Route both supervisor messages and child-process output through the bounded
# sink. Rotation is checked for every complete output line.
exec > >(log_sink) 2>&1

# Desktop environments can accidentally launch duplicate autostart entries.
# Keep exactly one supervisor for this login session.
exec 9> "$LOCK_FILE"
if ! flock -n 9; then
    echo "$(date --iso-8601=seconds) RustDesk supervisor is already running."
    exit 0
fi

cleanup() {
    "$WAYLAND_SCRIPT" stop || true
}

shutdown() {
    trap - EXIT INT TERM HUP
    cleanup
    exit 0
}

trap cleanup EXIT
trap shutdown INT TERM HUP

healthy() {
    pgrep -u "$UID" -f '/usr/share/rustdesk/rustdesk --service' >/dev/null &&
        pgrep -u "$UID" -f 'portal-screencast.py' >/dev/null &&
        pgrep -u "$UID" -f "^Xvfb ${VIRTUAL_DISPLAY} " >/dev/null &&
        pgrep -u "$UID" -f "^Xvfb ${GUI_DISPLAY} " >/dev/null
}

# Let the desktop session and portal backend settle after login.
sleep "$START_DELAY"

while true; do
    echo "$(date --iso-8601=seconds) Starting RustDesk stack."
    if "$WAYLAND_SCRIPT" start; then
        while healthy; do
            sleep "$HEALTH_INTERVAL"
        done
        echo "$(date --iso-8601=seconds) RustDesk stack became unhealthy."
    else
        echo "$(date --iso-8601=seconds) RustDesk stack failed to start."
    fi

    "$WAYLAND_SCRIPT" stop || true
    echo "$(date --iso-8601=seconds) Retrying in ${RETRY_DELAY}s."
    sleep "$RETRY_DELAY"
done
