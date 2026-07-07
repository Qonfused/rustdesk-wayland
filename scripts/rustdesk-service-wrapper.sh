#!/bin/bash
set -euo pipefail

# Let the desktop session and portal backend settle after login.
sleep "${RUSTDESK_AUTOSTART_DELAY:-5}"

exec "${HOME}/.local/bin/rustdesk-wayland.sh" start
