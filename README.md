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
| Screen capture | A Python script asks COSMIC for a PipeWire screencast, pipes it through GStreamer into Xvfb :99. An `ld.so.preload` shim lies to RustDesk about `DISPLAY` being `:99`. |
| Mouse | A fake `libxdo.so.4` replaces the real one. For RustDesk processes, it writes mouse events to `/dev/uinput` instead of X11. |
| Keyboard | The `ld.so.preload` shim also intercepts `XTestFakeKeyEvent` calls, converts the X11 keycodes to evdev, and writes them to `/dev/uinput`. |

The uinput devices show up as real input hardware, so the Wayland compositor picks them up normally.

```
Remote client
    │
    ▼
RustDesk --server (thinks DISPLAY=:99)
    │
    ├── Screen capture ──► reads from Xvfb :99
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

# hooks up ld.so.preload and swaps in the fake libxdo (needs sudo)
rustdesk-wayland.sh install
```

## Usage

```bash
rustdesk-wayland.sh start      # launches Xvfb, screencast, and RustDesk service
rustdesk-wayland.sh stop       # kills everything
rustdesk-wayland.sh uninstall  # undoes the install step
```

Set `RUSTDESK_SCREEN_RES` if your monitor isn't 2560x1080 (e.g., `RUSTDESK_SCREEN_RES=1920x1080x24`).

## Known issues

- You get a screen share dialog every time you run `start`. There is unfortunately no way around this as the portal requires consent.
- `portal-screencast.py` has to keep running. If it dies, the screen goes black.
- `/dev/uinput` gets `chmod 666` which is lazy; it's possible that a udev rule would be better.
- Screen dimensions are hardcoded in `libxdo_wrapper.c` (`SCREEN_W`/`SCREEN_H`). You can edit them if yours differ.

## Tested with

- Void Linux (glibc, x86_64)
- COSMIC DE
- RustDesk 1.3.x
