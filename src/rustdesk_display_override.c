#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * ld.so.preload shim for RustDesk on Wayland.
 *
 * Only activates for "rustdesk --server" processes. Does three things:
 *
 *   1. Lies about DISPLAY — returns ":99" (our Xvfb) so RustDesk captures
 *      from the virtual display where we're rendering the PipeWire screencast.
 *
 *   2. Fixes xdo_new() — RustDesk's libxdo would connect to :99 (useless),
 *      so we force it to connect to :0 (real XWayland) instead.
 *
 *   3. Hijacks XTestFakeKeyEvent() — RustDesk's keyboard input goes through
 *      XTest, which would send keystrokes to Xvfb (nowhere useful). We catch
 *      those calls and write the key events to /dev/uinput so the Wayland
 *      compositor actually sees them.
 */

static char *(*real_getenv)(const char *name) = NULL;
static int checked = 0;
static int is_rustdesk_server = 0;
static char real_display[64] = ":0";

/* xdo_t* xdo_new(const char *display) */
typedef void *(*xdo_new_fn)(const char *display);
static xdo_new_fn real_xdo_new = NULL;

/* XTestFakeKeyEvent from libXtst */
typedef int (*XTestFakeKeyEvent_fn)(void *display, unsigned int keycode,
                  int is_press, unsigned long delay);
static XTestFakeKeyEvent_fn real_XTestFakeKeyEvent = NULL;

/* uinput keyboard device for keyboard redirection */
static int kbd_fd = -1;
static int kbd_init_attempted = 0;

static void check_process(void) {
  if (checked)
    return;
  checked = 1;

  if (!real_getenv)
    real_getenv = dlsym(RTLD_NEXT, "getenv");

  /* Save the real DISPLAY */
  char *d = real_getenv("DISPLAY");
  if (d && strlen(d) < sizeof(real_display))
    strncpy(real_display, d, sizeof(real_display) - 1);

  char buf[4096];
  int fd = open("/proc/self/cmdline", O_RDONLY);
  if (fd < 0)
    return;

  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0)
    return;
  buf[n] = '\0';

  int has_rustdesk = 0;
  int has_server = 0;
  char *p = buf;
  char *end = buf + n;

  if (strstr(p, "rustdesk"))
    has_rustdesk = 1;

  while (p < end) {
    if (strcmp(p, "--server") == 0)
      has_server = 1;
    p += strlen(p) + 1;
  }

  is_rustdesk_server = has_rustdesk && has_server;
}

static void emit(int fd, int type, int code, int val) {
  struct input_event ev = {0};
  ev.type = type;
  ev.code = code;
  ev.value = val;
  write(fd, &ev, sizeof(ev));
}

static void syn(int fd) {
  emit(fd, EV_SYN, SYN_REPORT, 0);
}

static void init_kbd(void) {
  if (kbd_init_attempted)
    return;
  kbd_init_attempted = 1;

  kbd_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
  if (kbd_fd < 0) {
    fprintf(stderr, "shim: cannot open /dev/uinput for keyboard: %m\n");
    return;
  }

  ioctl(kbd_fd, UI_SET_EVBIT, EV_KEY);
  for (int i = 0; i < KEY_MAX; i++)
    ioctl(kbd_fd, UI_SET_KEYBIT, i);

  struct uinput_setup setup = {0};
  snprintf(setup.name, UINPUT_MAX_NAME_SIZE, "RustDesk Shim Keyboard");
  setup.id.bustype = BUS_VIRTUAL;
  setup.id.vendor = 0x1234;
  setup.id.product = 0x567a;
  ioctl(kbd_fd, UI_DEV_SETUP, &setup);
  ioctl(kbd_fd, UI_DEV_CREATE);

  usleep(200000); /* let compositor discover the device */
  fprintf(stderr, "shim: uinput keyboard device created\n");
}

char *getenv(const char *name) {
  if (!real_getenv)
    real_getenv = dlsym(RTLD_NEXT, "getenv");

  if (name) {
    if (strcmp(name, "DISPLAY") == 0) {
      check_process();
      if (is_rustdesk_server) {
        char *override = real_getenv("RUSTDESK_DISPLAY_OVERRIDE");
        if (override)
          return override;
        return ":99";
      }
    }

    /* We let RustDesk think it's X11 so screen capture works.
     * Keyboard is handled below by intercepting XTestFakeKeyEvent. */
  }

  return real_getenv(name);
}

/* xdo_new() would use DISPLAY=:99 (our Xvfb), which is wrong for input.
 * Point it at the real XWayland display instead. */
void *xdo_new(const char *display) {
  if (!real_xdo_new)
    real_xdo_new = dlsym(RTLD_NEXT, "xdo_new");

  check_process();
  if (is_rustdesk_server) {
    return real_xdo_new(real_display);
  }

  return real_xdo_new(display);
}

/* RustDesk sends keyboard input through XTestFakeKeyEvent (via rdev/enigo).
 * Without this, keystrokes go to Xvfb :99 and disappear.
 * We convert X11 keycodes to evdev (just subtract 8) and write to uinput. */
int XTestFakeKeyEvent(void *display, unsigned int keycode, int is_press,
            unsigned long delay) {
  check_process();

  if (is_rustdesk_server) {
    init_kbd();
    if (kbd_fd >= 0) {
      /* X11 keycode = evdev keycode + 8 */
      int evdev_code = (int)keycode - 8;
      if (evdev_code >= 0 && evdev_code < KEY_MAX) {
        emit(kbd_fd, EV_KEY, evdev_code, is_press ? 1 : 0);
        syn(kbd_fd);
      }
      return 1; /* success */
    }
  }

  /* Non-rustdesk processes: call real XTestFakeKeyEvent */
  if (!real_XTestFakeKeyEvent)
    real_XTestFakeKeyEvent = dlsym(RTLD_NEXT, "XTestFakeKeyEvent");
  if (real_XTestFakeKeyEvent)
    return real_XTestFakeKeyEvent(display, keycode, is_press, delay);
  return 0;
}
