#!/bin/bash
set -euo pipefail

# Setup script for RustDesk on Wayland. Run `make && make install` first.
# See README.md for prerequisites.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LIBDIR="${HOME}/.local/lib"
BINDIR="${HOME}/.local/bin"
AUTOSTART_DIR="${HOME}/.config/autostart"
SYSTEM_LIBDIR="${RUSTDESK_SYSTEM_LIBDIR:-/usr/local/lib/rustdesk-wayland}"

LOCAL_SO_PATH="${LIBDIR}/rustdesk_display_override.so"
LOCAL_XDO_WRAPPER="${LIBDIR}/libxdo_wrapper.so"
SERVICE_WRAPPER="${BINDIR}/rustdesk-service-wrapper.sh"
AUTOSTART_FILE="${AUTOSTART_DIR}/rustdesk-service.desktop"
SO_PATH="${SYSTEM_LIBDIR}/rustdesk_display_override.so"
XDO_WRAPPER="${SYSTEM_LIBDIR}/libxdo_wrapper.so"
PRELOAD_FILE="/etc/ld.so.preload"

# Screen resolution — adjust to match your monitor
SCREEN_RES="${RUSTDESK_SCREEN_RES:-2560x1080x24}"

# Keep RustDesk's virtual capture display away from MTGOBot's container Xvfb,
# which uses :99 and is visible from the host process table.
RUSTDESK_VIRTUAL_DISPLAY="${RUSTDESK_VIRTUAL_DISPLAY:-:98}"
if [[ "$RUSTDESK_VIRTUAL_DISPLAY" != :* ]]; then
    RUSTDESK_VIRTUAL_DISPLAY=":${RUSTDESK_VIRTUAL_DISPLAY}"
fi
RUSTDESK_PORTAL_LOG="${RUSTDESK_PORTAL_LOG:-/tmp/portal-screencast-${RUSTDESK_VIRTUAL_DISPLAY#:}.log}"

# RustDesk starts helper GUI processes (--tray/--cm) when a client connects.
# Keep those windows off the real compositor and off the captured display.
RUSTDESK_GUI_DISPLAY="${RUSTDESK_GUI_DISPLAY:-:97}"
if [[ "$RUSTDESK_GUI_DISPLAY" != :* ]]; then
    RUSTDESK_GUI_DISPLAY=":${RUSTDESK_GUI_DISPLAY}"
fi
RUSTDESK_GUI_SCREEN_RES="${RUSTDESK_GUI_SCREEN_RES:-1024x768x24}"

stop_virtual_display() {
    pkill -f "Xvfb ${RUSTDESK_VIRTUAL_DISPLAY}( |$)" 2>/dev/null || true
}

stop_gui_display() {
    pkill -f "Xvfb ${RUSTDESK_GUI_DISPLAY}( |$)" 2>/dev/null || true
}

stop_legacy_virtual_display() {
    # Clean up the old RustDesk-owned :99 Xvfb without touching MTGOBot's
    # container Xvfb, which has a different parent and screen size.
    [ "${RUSTDESK_STOP_LEGACY_DISPLAY:-1}" = "1" ] || return 0

    local pids pid ppid
    pids=$(pgrep -u "$(id -u)" -f '^Xvfb :99 -screen 0 2560x1080x24' 2>/dev/null || true)
    for pid in $pids; do
        ppid=$(ps -o ppid= -p "$pid" 2>/dev/null | tr -d '[:space:]')
        if [ "$ppid" = "1" ]; then
            kill "$pid" 2>/dev/null || true
        fi
    done
}

case "${1:-help}" in
    install)
        echo "=== Installing RustDesk Wayland workaround ==="
        sudo -v

        # Check prerequisites
        for cmd in Xvfb gst-launch-1.0 python3; do
            if ! command -v "$cmd" &>/dev/null; then
                echo "ERROR: $cmd not found. Install it first."
                exit 1
            fi
        done

        if [ ! -f "$LOCAL_SO_PATH" ] || [ ! -f "$LOCAL_XDO_WRAPPER" ] || [ ! -x "$SERVICE_WRAPPER" ]; then
            echo "ERROR: local shims or service wrapper not found. Run 'make install' first."
            exit 1
        fi

        # /etc/ld.so.preload is system-wide; keep the preload target out of
        # $HOME so services running as other users do not hit EACCES.
        echo "Installing preload shims to ${SYSTEM_LIBDIR}..."
        sudo install -d -m 755 "$SYSTEM_LIBDIR"
        sudo install -m 755 "$LOCAL_SO_PATH" "$SO_PATH"
        sudo install -m 755 "$LOCAL_XDO_WRAPPER" "$XDO_WRAPPER"

        # Add ld.so.preload shim
        if [ -f "$PRELOAD_FILE" ]; then
            sudo sed -i "\|${LOCAL_SO_PATH}|d;\|${SO_PATH}|d" "$PRELOAD_FILE"
        fi
        echo "Adding display override shim to $PRELOAD_FILE..."
        echo "$SO_PATH" | sudo tee -a "$PRELOAD_FILE" > /dev/null

        # Replace system libxdo with our wrapper (backup the original)
        if [ -f /usr/lib/libxdo.so.4 ] && [ ! -f /usr/lib/libxdo.so.4.real ]; then
            echo "Backing up /usr/lib/libxdo.so.4 → libxdo.so.4.real..."
            sudo mv /usr/lib/libxdo.so.4 /usr/lib/libxdo.so.4.real
        fi
        echo "Installing libxdo wrapper..."
        sudo cp "$XDO_WRAPPER" /usr/lib/libxdo.so.4
        sudo ldconfig 2>/dev/null || true

        # Install udev rule for /dev/uinput (replaces chmod 666)
        UDEV_RULE="${LIBDIR}/rustdesk-wayland/99-uinput.rules"
        if [ -f "$UDEV_RULE" ]; then
            echo "Installing udev rule for /dev/uinput..."
            sudo cp "$UDEV_RULE" /etc/udev/rules.d/99-uinput.rules
            sudo udevadm control --reload-rules
            sudo udevadm trigger /dev/uinput 2>/dev/null || true
        fi

        # Add user to input group for uinput access
        if ! id -nG | grep -qw input; then
            echo "Adding $(whoami) to input group..."
            sudo usermod -aG input "$(whoami)"
            echo "NOTE: Log out and back in for group change to take effect."
        fi

        echo "Installing desktop autostart entry..."
        mkdir -p "$AUTOSTART_DIR"
        cat > "$AUTOSTART_FILE" <<EOF
[Desktop Entry]
Type=Application
Name=RustDesk Wayland Service
Exec=${SERVICE_WRAPPER}
Hidden=false
NoDisplay=true
X-GNOME-Autostart-enabled=true
EOF

        echo ""
        echo "Installed. Run '$0 start' to launch."
        ;;

    start)
        echo "=== Starting RustDesk Wayland Setup ==="

        # Wait for xdg-desktop-portal to be ready (up to 5 minutes)
        echo "Waiting for xdg-desktop-portal..."
        for i in $(seq 1 300); do
            if gdbus call --session \
                --dest org.freedesktop.portal.Desktop \
                --object-path /org/freedesktop/portal/desktop \
                --method org.freedesktop.DBus.Properties.Get \
                org.freedesktop.portal.ScreenCast version \
                &>/dev/null; then
                break
            fi
            sleep 1
        done

        # Stop existing instances
        pkill -f 'rustdesk --service' 2>/dev/null || true
        pkill -f 'rustdesk --server' 2>/dev/null || true
        pkill -f 'portal-screencast.py' 2>/dev/null || true
        pkill -f 'gst-launch.*pipewiresrc' 2>/dev/null || true
        stop_virtual_display
        stop_gui_display
        stop_legacy_virtual_display
        rm -f /tmp/RustDesk/ipc_uinput_* 2>/dev/null || true
        rm -f "$RUSTDESK_PORTAL_LOG" 2>/dev/null || true
        sleep 2

        # Verify /dev/uinput is accessible (udev rule should handle this)
        if [ ! -w /dev/uinput ]; then
            echo "WARNING: /dev/uinput is not writable. Run '$0 install' first."
        fi

        # Start virtual X11 display for screen capture
        echo "Starting Xvfb on ${RUSTDESK_VIRTUAL_DISPLAY} (${SCREEN_RES})..."
        Xvfb "$RUSTDESK_VIRTUAL_DISPLAY" -screen 0 "$SCREEN_RES" &
        sleep 1

        # Hidden sink for RustDesk helper GUI windows, kept separate from the
        # capture display so popups do not leak into the remote session.
        echo "Starting hidden RustDesk GUI display on ${RUSTDESK_GUI_DISPLAY} (${RUSTDESK_GUI_SCREEN_RES})..."
        Xvfb "$RUSTDESK_GUI_DISPLAY" -screen 0 "$RUSTDESK_GUI_SCREEN_RES" &
        sleep 1

        # Launch portal screencast → GStreamer → Xvfb
        echo "Requesting screen share via portal..."
        DISPLAY="$RUSTDESK_VIRTUAL_DISPLAY" nohup /usr/bin/python3 "${BINDIR}/portal-screencast.py" \
            > "$RUSTDESK_PORTAL_LOG" 2>&1 &

        for i in $(seq 1 30); do
            if grep -q "Pipeline running" "$RUSTDESK_PORTAL_LOG" 2>/dev/null; then
                break
            fi
            sleep 1
        done

        if ! grep -q "Pipeline running" "$RUSTDESK_PORTAL_LOG" 2>/dev/null; then
            echo "ERROR: Portal screencast did not start."
            cat "$RUSTDESK_PORTAL_LOG"
            exit 1
        fi

        echo "Screencast pipeline running."

        # Start RustDesk service
        echo "Starting RustDesk service..."
        export RUSTDESK_DISPLAY_OVERRIDE="${RUSTDESK_DISPLAY_OVERRIDE:-$RUSTDESK_VIRTUAL_DISPLAY}"
        export RUSTDESK_GUI_DISPLAY
        nohup /usr/share/rustdesk/rustdesk --service > /dev/null 2>&1 &
        sleep 2

        echo ""
        echo "=== Ready! Connect from your remote machine. ==="
        ;;

    stop)
        echo "=== Stopping RustDesk Wayland Setup ==="
        pkill -f 'rustdesk --service' 2>/dev/null || true
        pkill -f 'rustdesk --server' 2>/dev/null || true
        pkill -f 'portal-screencast.py' 2>/dev/null || true
        pkill -f 'gst-launch.*pipewiresrc' 2>/dev/null || true
        stop_virtual_display
        stop_gui_display
        stop_legacy_virtual_display
        echo "Stopped."
        ;;

    uninstall)
        echo "=== Uninstalling RustDesk Wayland workaround ==="
        sudo -v

        # Remove ld.so.preload entry
        if [ -f "$PRELOAD_FILE" ]; then
            sudo sed -i "\|${LOCAL_SO_PATH}|d;\|${SO_PATH}|d" "$PRELOAD_FILE"
            [ -s "$PRELOAD_FILE" ] || sudo rm -f "$PRELOAD_FILE"
            echo "Removed shim from $PRELOAD_FILE."
        fi

        # Restore original libxdo
        if [ -f /usr/lib/libxdo.so.4.real ]; then
            echo "Restoring original libxdo.so.4..."
            sudo mv /usr/lib/libxdo.so.4.real /usr/lib/libxdo.so.4
            sudo ldconfig 2>/dev/null || true
        fi

        if [ -d "$SYSTEM_LIBDIR" ]; then
            sudo rm -f "$SO_PATH" "$XDO_WRAPPER"
            sudo rmdir "$SYSTEM_LIBDIR" 2>/dev/null || true
        fi

        if [ -f "$AUTOSTART_FILE" ]; then
            rm -f "$AUTOSTART_FILE"
            echo "Removed desktop autostart entry."
        fi

        # Remove udev rule
        if [ -f /etc/udev/rules.d/99-uinput.rules ]; then
            echo "Removing udev rule..."
            sudo rm -f /etc/udev/rules.d/99-uinput.rules
            sudo udevadm control --reload-rules
        fi

        echo "Uninstalled. Run '$0 stop' to stop running services."
        ;;

    *)
        echo "Usage: $0 {install|start|stop|uninstall}"
        echo ""
        echo "  install    Set up ld.so.preload shim and libxdo wrapper"
        echo "  start      Launch Xvfb, portal screencast, and RustDesk service"
        echo "  stop       Stop all RustDesk workaround processes"
        echo "  uninstall  Remove shim and restore original libxdo"
        echo ""
        echo "Environment variables:"
        echo "  RUSTDESK_SCREEN_RES  Screen resolution (default: 2560x1080x24)"
        echo "  RUSTDESK_VIRTUAL_DISPLAY  Capture display (default: :98)"
        echo "  RUSTDESK_GUI_DISPLAY  Hidden helper GUI display (default: :97)"
        exit 1
        ;;
esac
