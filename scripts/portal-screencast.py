#!/usr/bin/env python3
"""
Grabs a PipeWire screencast from xdg-desktop-portal and renders it into
an X11 display (Xvfb) via GStreamer. Must stay running — if it exits, the
portal session dies and the screen goes black.

Usage: DISPLAY=:99 portal-screencast.py [--print-only]
"""

import os
import signal
import subprocess
import sys

from gi.repository import GLib, Gio

bus = Gio.bus_get_sync(Gio.BusType.SESSION)
unique_name = bus.get_unique_name()
sender = unique_name.lstrip(':').replace('.', '_')

token = f"rd_{GLib.get_monotonic_time()}"
session_path = f"/org/freedesktop/portal/desktop/session/{sender}/{token}"

loop = GLib.MainLoop()
node_id = None
gst_proc = None
print_only = '--print-only' in sys.argv


def cleanup(*args):
    if gst_proc and gst_proc.poll() is None:
        gst_proc.terminate()
    loop.quit()
    sys.exit(0)

signal.signal(signal.SIGINT, cleanup)
signal.signal(signal.SIGTERM, cleanup)


def launch_gstreamer(nid):
    global gst_proc
    display = os.environ.get('DISPLAY', ':99')
    cmd = [
        'gst-launch-1.0',
        f'pipewiresrc', f'path={nid}', '!',
        'videoconvert', '!',
        f'ximagesink', 'sync=false',
    ]
    print(f"Launching GStreamer on DISPLAY={display}: {' '.join(cmd)}", file=sys.stderr)
    gst_proc = subprocess.Popen(cmd, env={**os.environ, 'DISPLAY': display})


def on_response(connection, sender_name, object_path, interface_name, signal_name, parameters):
    global node_id
    response, results = parameters.unpack()

    if 'session_handle' in results:
        print(f"Session created: {results['session_handle']}", file=sys.stderr)
        select_sources()
        return

    if signal_name == 'Response' and 'streams' not in results and response == 0:
        print("Sources selected, starting screencast...", file=sys.stderr)
        start_screencast()
        return

    if 'streams' in results:
        streams = results['streams']
        if streams:
            node_id = streams[0][0]
            print(f"PipeWire node ID: {node_id}", file=sys.stderr)
            print(node_id, flush=True)
            if not print_only:
                launch_gstreamer(node_id)
                print("Pipeline running. Keeping session alive (Ctrl+C to stop).", file=sys.stderr)
            else:
                print("Session alive. Ctrl+C to stop.", file=sys.stderr)
            # Don't quit the loop — keep the session alive
            return

    if response != 0:
        print(f"Portal error: response={response}", file=sys.stderr)
        loop.quit()


def create_session():
    bus.call_sync(
        'org.freedesktop.portal.Desktop',
        '/org/freedesktop/portal/desktop',
        'org.freedesktop.portal.ScreenCast',
        'CreateSession',
        GLib.Variant('(a{sv})', ({
            'session_handle_token': GLib.Variant('s', token),
            'handle_token': GLib.Variant('s', f'{token}_cs'),
        },)),
        None, Gio.DBusCallFlags.NONE, -1, None,
    )


def select_sources():
    bus.call_sync(
        'org.freedesktop.portal.Desktop',
        '/org/freedesktop/portal/desktop',
        'org.freedesktop.portal.ScreenCast',
        'SelectSources',
        GLib.Variant('(oa{sv})', (
            session_path,
            {
                'types': GLib.Variant('u', 1),
                'multiple': GLib.Variant('b', False),
                'handle_token': GLib.Variant('s', f'{token}_ss'),
            },
        )),
        None, Gio.DBusCallFlags.NONE, -1, None,
    )
    print(">>> Select your monitor in the dialog and click Share <<<", file=sys.stderr)


def start_screencast():
    bus.call_sync(
        'org.freedesktop.portal.Desktop',
        '/org/freedesktop/portal/desktop',
        'org.freedesktop.portal.ScreenCast',
        'Start',
        GLib.Variant('(osa{sv})', (
            session_path, '',
            {'handle_token': GLib.Variant('s', f'{token}_st')},
        )),
        None, Gio.DBusCallFlags.NONE, -1, None,
    )


# Watch for GStreamer process exit
def check_gst():
    if gst_proc and gst_proc.poll() is not None:
        print(f"GStreamer exited with code {gst_proc.returncode}", file=sys.stderr)
        loop.quit()
        return False
    return True


bus.signal_subscribe(
    'org.freedesktop.portal.Desktop',
    'org.freedesktop.portal.Request',
    'Response',
    None, None,
    Gio.DBusSignalFlags.NONE,
    on_response,
)

# No timeout — keep session alive indefinitely
if not print_only:
    GLib.timeout_add_seconds(2, check_gst)

print("Creating screencast session...", file=sys.stderr)
create_session()

loop.run()

if gst_proc and gst_proc.poll() is None:
    gst_proc.terminate()

sys.exit(0 if node_id else 1)
