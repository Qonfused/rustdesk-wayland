# rustdesk-wayland

RustDesk doesn't work on Wayland. This is a workaround that makes it work anyway, using `ld.so.preload` shims, a fake libxdo, and PipeWire portal screencasting.

Tested on Void Linux with COSMIC DE, but should work with other Wayland compositors that have `xdg-desktop-portal` support.

## The problem

RustDesk assumes X11 for everything. On Wayland, three things break:

1. **Screen capture** — XWayland runs rootless, so there's no root window to capture. RustDesk gets a black screen.
2. **Mouse** — RustDesk sends mouse events through libxdo/X11, but X11 input doesn't reach the Wayland compositor.
3. **Keyboard** — RustDesk sends key events through XTest, which go to the fake Xvfb display and vanish.

## How it works

The workaround has three parts, one for each broken thing:

| What's broken | Fix |
|---|---|
| Screen capture | A Python script asks COSMIC for a PipeWire screencast, pipes it through GStreamer into Xvfb :98. An `ld.so.preload` shim lies to RustDesk about `DISPLAY` being `:98`. |
| Mouse | A fake `libxdo.so.4` replaces the real one. For RustDesk processes, it writes mouse events to `/dev/uinput` instead of X11. |
| Keyboard | The `ld.so.preload` shim also intercepts `XTestFakeKeyEvent` calls, converts the X11 keycodes to evdev, and writes them to `/dev/uinput`. |
| Helper GUI windows | RustDesk's `--tray` and `--cm` helper processes are pointed at a hidden Xvfb `:97` so their windows do not flash into the real Wayland session or disturb tiling. |

The uinput devices show up as real input hardware, so the Wayland compositor picks them up normally.

```
Remote client
    │
    ▼
RustDesk --server (thinks DISPLAY=:98)
    │
    ├── Screen capture ──► reads from Xvfb :98
    │                           ▲
    │                      GStreamer pipewiresrc → ximagesink
    │                           ▲
    │                      portal-screencast.py (keeps the PipeWire session alive)
    │                           ▲
    │                      Wayland compositor
    │
    ├── Mouse ──► fake libxdo ──► /dev/uinput ──► compositor
    │
    └── Keyboard ──► XTestFakeKeyEvent ──► shim ──► /dev/uinput ──► compositor
```

## What's in here

- `src/rustdesk_display_override.c` — the `ld.so.preload` shim. Lies about DISPLAY, redirects `xdo_new()` to the real X display, and intercepts `XTestFakeKeyEvent` for keyboard.
- `src/libxdo_wrapper.c` — replaces `libxdo.so.4`. For RustDesk, mouse events go through uinput. For everything else, it forwards to the real libxdo.
- `scripts/portal-screencast.py` — asks xdg-desktop-portal for a screencast and renders it into Xvfb with GStreamer. Has to stay running or the PipeWire session dies.
- `scripts/rustdesk-wayland.sh` — does the actual setup/teardown.
- `scripts/rustdesk-service-wrapper.sh` — login autostart wrapper that waits briefly, then runs `rustdesk-wayland.sh start`.

## Prerequisites

You need these installed:

- RustDesk (extracted from `.deb` or however you get it on your distro)
- xdotool — `xbps-install xdotool`
- Xvfb — `xbps-install xorg-server-xvfb`
- GStreamer with PipeWire — `xbps-install gst-plugins-good1`
- Python 3 with PyGObject — `xbps-install python3-gobject`
- An xdg-desktop-portal backend (COSMIC ships one, GNOME/KDE have their own)

## Setup

```bash
make
make install

# Deploy udev rule and add user to input group (one-time, needs sudo)
rustdesk-wayland.sh install

# Log out and back in so the input group membership takes effect
# (Or you can use: newgrp input)
```

The setup command copies the preload shim into `/usr/local/lib/rustdesk-wayland`
before adding it to `/etc/ld.so.preload`, installs the udev rule, replaces
`libxdo.so.4`, and creates `~/.config/autostart/rustdesk-service.desktop`.
Do not point `/etc/ld.so.preload` at a file under `~/.local/lib`; system startup
jobs running as other users may be unable to read through your home directory
and will print loader errors.

## Usage

```bash
rustdesk-wayland.sh start      # launches Xvfb, screencast, and RustDesk service
rustdesk-wayland.sh stop       # kills everything
rustdesk-wayland.sh uninstall  # undoes the install step
```

Set `RUSTDESK_SCREEN_RES` if your monitor isn't 2560x1080 (e.g., `RUSTDESK_SCREEN_RES=1920x1080x24`). The virtual capture display defaults to `:98` so it does not collide with MTGOBot's `:99` Xvfb; override it with `RUSTDESK_VIRTUAL_DISPLAY` only if needed.
RustDesk helper GUI windows default to a separate hidden `:97` display; override
that with `RUSTDESK_GUI_DISPLAY` only if needed.

### Autostart

Once installed, RustDesk will automatically launch the full workaround (including screen capture) on login via `~/.config/autostart/rustdesk-service.desktop`. The autostart wrapper calls `rustdesk-wayland.sh start` in the background after a 5-second delay to let the desktop session stabilize.

## Known issues

- **Screen share dialog:** The first time you run `start` after install, you'll get a COSMIC screen share consent dialog. Click "Share" to proceed. The permission is saved in `~/.local/share/rustdesk-wayland/restore_token` and reused on subsequent runs — no more dialogs (unless the token expires or is revoked).
- **`portal-screencast.py` must stay running.** If it dies, the screen goes black. The autostart wrapper ensures it starts on login.
- **Input group membership:** After `make install && rustdesk-wayland.sh install`, you must log out and back in (or `newgrp input`) for the `input` group to take effect. Without it, `/dev/uinput` is inaccessible and mouse/keyboard won't work. The udev rule (`/etc/udev/rules.d/99-uinput.rules`) handles permissions permanently.
- **RustDesk client keyboard setting:** Some RustDesk clients (observed on macOS) have a "Block keyboard input" option in their remote settings. Verify this is **unchecked** on the client side, otherwise keyboard input will be ignored even if the workaround is working.
- **Screen dimensions:** Hardcoded in `libxdo_wrapper.c` (`SCREEN_W`/`SCREEN_H`). Edit them if your monitor differs from 2560×1080. Use `RUSTDESK_SCREEN_RES=WIDTHxHEIGHTx24` when running `start` to match.

## Tested with

- Void Linux (glibc, x86_64)
- COSMIC DE
- RustDesk 1.3.x
