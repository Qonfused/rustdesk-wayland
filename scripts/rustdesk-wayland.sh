#!/bin/bash
set -euo pipefail

# Setup script for RustDesk on Wayland. Run `make && make install` first.
# See README.md for prerequisites.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LIBDIR="${HOME}/.local/lib"
BINDIR="${HOME}/.local/bin"

SO_PATH="${LIBDIR}/rustdesk_display_override.so"
XDO_WRAPPER="${LIBDIR}/libxdo_wrapper.so"
PRELOAD_FILE="/etc/ld.so.preload"

# Screen resolution — adjust to match your monitor
SCREEN_RES="${RUSTDESK_SCREEN_RES:-2560x1080x24}"

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

        if [ ! -f "$SO_PATH" ]; then
            echo "ERROR: $SO_PATH not found. Run 'make install' first."
            exit 1
        fi

        # Add ld.so.preload shim
        if ! grep -qF "$SO_PATH" "$PRELOAD_FILE" 2>/dev/null; then
            echo "Adding display override shim to $PRELOAD_FILE..."
            echo "$SO_PATH" | sudo tee -a "$PRELOAD_FILE" > /dev/null
        fi

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

        echo ""
        echo "Installed. Run '$0 start' to launch."
        ;;

    start)
        echo "=== Starting RustDesk Wayland Setup ==="

        # Stop existing instances
        pkill -f 'rustdesk --service' 2>/dev/null || true
        pkill -f 'rustdesk --server' 2>/dev/null || true
        pkill -f 'portal-screencast.py' 2>/dev/null || true
        pkill -f 'gst-launch.*pipewiresrc' 2>/dev/null || true
        pkill -f 'Xvfb :99' 2>/dev/null || true
        rm -f /tmp/RustDesk/ipc_uinput_* 2>/dev/null || true
        sleep 2

        # Verify /dev/uinput is accessible (udev rule should handle this)
        if [ ! -w /dev/uinput ]; then
            echo "WARNING: /dev/uinput is not writable. Run '$0 install' first."
        fi

        # Start virtual X11 display for screen capture
        echo "Starting Xvfb on :99 (${SCREEN_RES})..."
        Xvfb :99 -screen 0 "$SCREEN_RES" &
        sleep 1

        # Launch portal screencast → GStreamer → Xvfb
        echo "Requesting screen share via portal..."
        DISPLAY=:99 nohup /usr/bin/python3 "${BINDIR}/portal-screencast.py" \
            > /tmp/portal-screencast.log 2>&1 &

        for i in $(seq 1 30); do
            if grep -q "Pipeline running" /tmp/portal-screencast.log 2>/dev/null; then
                break
            fi
            sleep 1
        done

        if ! grep -q "Pipeline running" /tmp/portal-screencast.log 2>/dev/null; then
            echo "ERROR: Portal screencast did not start."
            cat /tmp/portal-screencast.log
            exit 1
        fi

        echo "Screencast pipeline running."

        # Start RustDesk service
        echo "Starting RustDesk service..."
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
        pkill -f 'Xvfb :99' 2>/dev/null || true
        echo "Stopped."
        ;;

    uninstall)
        echo "=== Uninstalling RustDesk Wayland workaround ==="
        sudo -v

        # Remove ld.so.preload entry
        if [ -f "$PRELOAD_FILE" ]; then
            sudo sed -i "\|${SO_PATH}|d" "$PRELOAD_FILE"
            [ -s "$PRELOAD_FILE" ] || sudo rm -f "$PRELOAD_FILE"
            echo "Removed shim from $PRELOAD_FILE."
        fi

        # Restore original libxdo
        if [ -f /usr/lib/libxdo.so.4.real ]; then
            echo "Restoring original libxdo.so.4..."
            sudo mv /usr/lib/libxdo.so.4.real /usr/lib/libxdo.so.4
            sudo ldconfig 2>/dev/null || true
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
        exit 1
        ;;
esac
