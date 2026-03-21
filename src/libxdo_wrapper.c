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
 * Fake libxdo.so.4 for RustDesk on Wayland.
 *
 * Replaces the system xdotool library. For "rustdesk --server" processes,
 * mouse events go through /dev/uinput instead of X11 (which can't reach
 * the Wayland compositor). Everything else forwards to the real libxdo
 * at /usr/lib/libxdo.so.4.real.
 *
 * The keyboard functions here are fully implemented but RustDesk doesn't
 * call them in X11 mode — it uses XTest instead, which the ld.so.preload
 * shim handles. They're here as a fallback.
 */

#define SCREEN_W 2560
#define SCREEN_H 1080

/* Our fake xdo context */
typedef struct {
  int mouse_fd;    /* uinput device for mouse */
  int kbd_fd;      /* uinput device for keyboard */
  int use_uinput;  /* 1 if rustdesk --server */
  void *real_xdo;  /* real xdo context for non-rustdesk */
} wrapper_xdo_t;

static void *real_libxdo = NULL;
static int _is_rustdesk = -1;

static void ensure_real_libxdo(void) {
  if (real_libxdo) return;
  real_libxdo = dlopen("/usr/lib/libxdo.so.4.real", RTLD_NOW | RTLD_LOCAL);
}

static int is_rustdesk_server(void) {
  if (_is_rustdesk >= 0) return _is_rustdesk;
  _is_rustdesk = 0;

  char buf[4096];
  int fd = open("/proc/self/cmdline", O_RDONLY);
  if (fd < 0) return 0;
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n <= 0) return 0;
  buf[n] = '\0';

  int has_rustdesk = strstr(buf, "rustdesk") != NULL;
  int has_server = 0;
  char *p = buf, *end = buf + n;
  while (p < end) {
    if (strcmp(p, "--server") == 0) has_server = 1;
    p += strlen(p) + 1;
  }
  _is_rustdesk = has_rustdesk && has_server;
  return _is_rustdesk;
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

static int create_mouse_device(void) {
  int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
  if (fd < 0) {
    fprintf(stderr, "libxdo_wrapper: cannot open /dev/uinput: %m\n");
    return -1;
  }

  ioctl(fd, UI_SET_EVBIT, EV_KEY);
  ioctl(fd, UI_SET_KEYBIT, BTN_LEFT);
  ioctl(fd, UI_SET_KEYBIT, BTN_RIGHT);
  ioctl(fd, UI_SET_KEYBIT, BTN_MIDDLE);
  ioctl(fd, UI_SET_EVBIT, EV_ABS);
  ioctl(fd, UI_SET_ABSBIT, ABS_X);
  ioctl(fd, UI_SET_ABSBIT, ABS_Y);
  ioctl(fd, UI_SET_EVBIT, EV_REL);
  ioctl(fd, UI_SET_RELBIT, REL_WHEEL);
  ioctl(fd, UI_SET_RELBIT, REL_HWHEEL);

  struct uinput_abs_setup abs_x = {0};
  abs_x.code = ABS_X;
  abs_x.absinfo.minimum = 0;
  abs_x.absinfo.maximum = SCREEN_W - 1;
  ioctl(fd, UI_ABS_SETUP, &abs_x);

  struct uinput_abs_setup abs_y = {0};
  abs_y.code = ABS_Y;
  abs_y.absinfo.minimum = 0;
  abs_y.absinfo.maximum = SCREEN_H - 1;
  ioctl(fd, UI_ABS_SETUP, &abs_y);

  struct uinput_setup setup = {0};
  snprintf(setup.name, UINPUT_MAX_NAME_SIZE, "RustDesk Virtual Mouse");
  setup.id.bustype = BUS_VIRTUAL;
  setup.id.vendor = 0x1234;
  setup.id.product = 0x5678;
  ioctl(fd, UI_DEV_SETUP, &setup);
  ioctl(fd, UI_DEV_CREATE);

  usleep(100000); /* let compositor discover the device */
  return fd;
}

static int create_kbd_device(void) {
  int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
  if (fd < 0) {
    fprintf(stderr, "libxdo_wrapper: cannot open /dev/uinput for kbd: %m\n");
    return -1;
  }

  ioctl(fd, UI_SET_EVBIT, EV_KEY);
  /* Enable all key codes */
  for (int i = 0; i < KEY_MAX; i++) {
    ioctl(fd, UI_SET_KEYBIT, i);
  }

  struct uinput_setup setup = {0};
  snprintf(setup.name, UINPUT_MAX_NAME_SIZE, "RustDesk Virtual Keyboard");
  setup.id.bustype = BUS_VIRTUAL;
  setup.id.vendor = 0x1234;
  setup.id.product = 0x5679;
  ioctl(fd, UI_DEV_SETUP, &setup);
  ioctl(fd, UI_DEV_CREATE);

  usleep(100000);
  return fd;
}

/* === xdo API implementation === */

void *xdo_new(const char *display) {
  wrapper_xdo_t *w = calloc(1, sizeof(wrapper_xdo_t));
  if (!w) return NULL;

  if (is_rustdesk_server()) {
    w->use_uinput = 1;
    w->mouse_fd = create_mouse_device();
    w->kbd_fd = create_kbd_device();
    if (w->mouse_fd < 0 || w->kbd_fd < 0) {
      fprintf(stderr, "libxdo_wrapper: uinput setup failed, falling back to real xdo\n");
      w->use_uinput = 0;
      if (w->mouse_fd >= 0) { ioctl(w->mouse_fd, UI_DEV_DESTROY); close(w->mouse_fd); }
      if (w->kbd_fd >= 0) { ioctl(w->kbd_fd, UI_DEV_DESTROY); close(w->kbd_fd); }
      ensure_real_libxdo();
      if (real_libxdo) {
        typedef void *(*fn_t)(const char *);
        fn_t real_fn = dlsym(real_libxdo, "xdo_new");
        if (real_fn) w->real_xdo = real_fn(display);
      }
    } else {
      fprintf(stderr, "libxdo_wrapper: uinput devices created successfully\n");
    }
  } else {
    ensure_real_libxdo();
    if (real_libxdo) {
      typedef void *(*fn_t)(const char *);
      fn_t real_fn = dlsym(real_libxdo, "xdo_new");
      if (real_fn) w->real_xdo = real_fn(display);
    }
  }

  return w;
}

void *xdo_new_with_opened_display(void *display, const char *close_display, int quiet) {
  if (is_rustdesk_server()) {
    return xdo_new(NULL);
  }
  ensure_real_libxdo();
  if (!real_libxdo) return NULL;
  typedef void *(*fn_t)(void *, const char *, int);
  fn_t real_fn = dlsym(real_libxdo, "xdo_new_with_opened_display");
  if (!real_fn) return NULL;

  wrapper_xdo_t *w = calloc(1, sizeof(wrapper_xdo_t));
  if (!w) return NULL;
  w->real_xdo = real_fn(display, close_display, quiet);
  return w;
}

void xdo_free(void *xdo) {
  wrapper_xdo_t *w = (wrapper_xdo_t *)xdo;
  if (!w) return;
  if (w->use_uinput) {
    if (w->mouse_fd >= 0) { ioctl(w->mouse_fd, UI_DEV_DESTROY); close(w->mouse_fd); }
    if (w->kbd_fd >= 0) { ioctl(w->kbd_fd, UI_DEV_DESTROY); close(w->kbd_fd); }
  } else if (w->real_xdo) {
    ensure_real_libxdo();
    if (real_libxdo) {
      typedef void (*fn_t)(void *);
      fn_t real_fn = dlsym(real_libxdo, "xdo_free");
      if (real_fn) real_fn(w->real_xdo);
    }
  }
  free(w);
}

int xdo_move_mouse(void *xdo, int x, int y, int screen) {
  wrapper_xdo_t *w = (wrapper_xdo_t *)xdo;
  if (!w) return 1;
  if (w->use_uinput) {
    emit(w->mouse_fd, EV_ABS, ABS_X, x);
    emit(w->mouse_fd, EV_ABS, ABS_Y, y);
    syn(w->mouse_fd);
    return 0;
  }
  ensure_real_libxdo();
  if (!real_libxdo || !w->real_xdo) return 1;
  typedef int (*fn_t)(void *, int, int, int);
  return ((fn_t)dlsym(real_libxdo, "xdo_move_mouse"))(w->real_xdo, x, y, screen);
}

int xdo_move_mouse_relative(void *xdo, int x, int y) {
  wrapper_xdo_t *w = (wrapper_xdo_t *)xdo;
  if (!w) return 1;
  if (w->use_uinput) {
    emit(w->mouse_fd, EV_REL, REL_X, x);
    emit(w->mouse_fd, EV_REL, REL_Y, y);
    syn(w->mouse_fd);
    return 0;
  }
  ensure_real_libxdo();
  if (!real_libxdo || !w->real_xdo) return 1;
  typedef int (*fn_t)(void *, int, int);
  return ((fn_t)dlsym(real_libxdo, "xdo_move_mouse_relative"))(w->real_xdo, x, y);
}

static int button_to_code(int button) {
  switch (button) {
    case 1: return BTN_LEFT;
    case 2: return BTN_MIDDLE;
    case 3: return BTN_RIGHT;
    case 4: return BTN_SIDE;
    case 5: return BTN_EXTRA;
    default: return BTN_LEFT;
  }
}

int xdo_mouse_down(void *xdo, unsigned long window, int button) {
  wrapper_xdo_t *w = (wrapper_xdo_t *)xdo;
  if (!w) return 1;
  if (w->use_uinput) {
    /* Scroll wheel: button 4=up, 5=down */
    if (button == 4) { emit(w->mouse_fd, EV_REL, REL_WHEEL, 1); syn(w->mouse_fd); return 0; }
    if (button == 5) { emit(w->mouse_fd, EV_REL, REL_WHEEL, -1); syn(w->mouse_fd); return 0; }
    emit(w->mouse_fd, EV_KEY, button_to_code(button), 1);
    syn(w->mouse_fd);
    return 0;
  }
  ensure_real_libxdo();
  if (!real_libxdo || !w->real_xdo) return 1;
  typedef int (*fn_t)(void *, unsigned long, int);
  return ((fn_t)dlsym(real_libxdo, "xdo_mouse_down"))(w->real_xdo, window, button);
}

int xdo_mouse_up(void *xdo, unsigned long window, int button) {
  wrapper_xdo_t *w = (wrapper_xdo_t *)xdo;
  if (!w) return 1;
  if (w->use_uinput) {
    if (button == 4 || button == 5) return 0; /* scroll has no "up" */
    emit(w->mouse_fd, EV_KEY, button_to_code(button), 0);
    syn(w->mouse_fd);
    return 0;
  }
  ensure_real_libxdo();
  if (!real_libxdo || !w->real_xdo) return 1;
  typedef int (*fn_t)(void *, unsigned long, int);
  return ((fn_t)dlsym(real_libxdo, "xdo_mouse_up"))(w->real_xdo, window, button);
}

int xdo_click_window(void *xdo, unsigned long window, int button) {
  xdo_mouse_down(xdo, window, button);
  usleep(10000);
  xdo_mouse_up(xdo, window, button);
  return 0;
}

/* Key format handling. xdo sends keys as:
 *   - keysym names: "Return", "Control_L", "F1", etc.
 *   - unicode: "U41" = 'A', "U20" = space
 *   - raw X11 keycodes: "111" (evdev + 8)
 * All get mapped to Linux evdev keycodes. */

#include <ctype.h>

/* Map Unicode codepoint to Linux keycode + shift flag */
static int unicode_to_linux(unsigned int cp, int *need_shift) {
  *need_shift = 0;
  /* Lowercase letters */
  if (cp >= 'a' && cp <= 'z') return KEY_A + (cp - 'a');
  /* Uppercase letters */
  if (cp >= 'A' && cp <= 'Z') { *need_shift = 1; return KEY_A + (cp - 'A'); }
  /* Digits */
  if (cp >= '1' && cp <= '9') return KEY_1 + (cp - '1');
  if (cp == '0') return KEY_0;
  /* Common chars */
  if (cp == ' ') return KEY_SPACE;
  if (cp == '\n' || cp == '\r') return KEY_ENTER;
  if (cp == '\t') return KEY_TAB;
  if (cp == '-') return KEY_MINUS;
  if (cp == '=') return KEY_EQUAL;
  if (cp == '[') return KEY_LEFTBRACE;
  if (cp == ']') return KEY_RIGHTBRACE;
  if (cp == ';') return KEY_SEMICOLON;
  if (cp == '\'') return KEY_APOSTROPHE;
  if (cp == '`') return KEY_GRAVE;
  if (cp == '\\') return KEY_BACKSLASH;
  if (cp == ',') return KEY_COMMA;
  if (cp == '.') return KEY_DOT;
  if (cp == '/') return KEY_SLASH;
  /* Shifted chars */
  if (cp == '!') { *need_shift = 1; return KEY_1; }
  if (cp == '@') { *need_shift = 1; return KEY_2; }
  if (cp == '#') { *need_shift = 1; return KEY_3; }
  if (cp == '$') { *need_shift = 1; return KEY_4; }
  if (cp == '%') { *need_shift = 1; return KEY_5; }
  if (cp == '^') { *need_shift = 1; return KEY_6; }
  if (cp == '&') { *need_shift = 1; return KEY_7; }
  if (cp == '*') { *need_shift = 1; return KEY_8; }
  if (cp == '(') { *need_shift = 1; return KEY_9; }
  if (cp == ')') { *need_shift = 1; return KEY_0; }
  if (cp == '_') { *need_shift = 1; return KEY_MINUS; }
  if (cp == '+') { *need_shift = 1; return KEY_EQUAL; }
  if (cp == '{') { *need_shift = 1; return KEY_LEFTBRACE; }
  if (cp == '}') { *need_shift = 1; return KEY_RIGHTBRACE; }
  if (cp == ':') { *need_shift = 1; return KEY_SEMICOLON; }
  if (cp == '"') { *need_shift = 1; return KEY_APOSTROPHE; }
  if (cp == '~') { *need_shift = 1; return KEY_GRAVE; }
  if (cp == '|') { *need_shift = 1; return KEY_BACKSLASH; }
  if (cp == '<') { *need_shift = 1; return KEY_COMMA; }
  if (cp == '>') { *need_shift = 1; return KEY_DOT; }
  if (cp == '?') { *need_shift = 1; return KEY_SLASH; }
  return -1;
}

/* Convert keysym name to Linux keycode. Returns -1 and sets *need_shift=0 on failure. */
static int keysym_name_to_linux(const char *name, int *need_shift) {
  *need_shift = 0;

  /* Unicode format: "U{HEX}" */
  if (name[0] == 'U' && strlen(name) >= 2 && isxdigit(name[1])) {
    unsigned int cp = 0;
    if (sscanf(name + 1, "%x", &cp) == 1) {
      return unicode_to_linux(cp, need_shift);
    }
  }

  /* Raw keycode (decimal number) — these are X11 keycodes, roughly offset by 8 from evdev */
  if (isdigit(name[0])) {
    int xkeycode = atoi(name);
    if (xkeycode >= 8) return xkeycode - 8; /* X11 keycode = evdev + 8 */
    return xkeycode;
  }

  /* Single character */
  if (strlen(name) == 1) {
    return unicode_to_linux((unsigned char)name[0], need_shift);
  }

  /* Named keys */
  if (strcmp(name, "Return") == 0) return KEY_ENTER;
  if (strcmp(name, "KP_Enter") == 0) return KEY_KPENTER;
  if (strcmp(name, "space") == 0) return KEY_SPACE;
  if (strcmp(name, "BackSpace") == 0) return KEY_BACKSPACE;
  if (strcmp(name, "Tab") == 0) return KEY_TAB;
  if (strcmp(name, "Escape") == 0) return KEY_ESC;
  if (strcmp(name, "Delete") == 0) return KEY_DELETE;
  if (strcmp(name, "Home") == 0) return KEY_HOME;
  if (strcmp(name, "End") == 0) return KEY_END;
  if (strcmp(name, "Page_Up") == 0) return KEY_PAGEUP;
  if (strcmp(name, "Page_Down") == 0) return KEY_PAGEDOWN;
  if (strcmp(name, "Left") == 0) return KEY_LEFT;
  if (strcmp(name, "Right") == 0) return KEY_RIGHT;
  if (strcmp(name, "Up") == 0) return KEY_UP;
  if (strcmp(name, "Down") == 0) return KEY_DOWN;
  /* Modifiers */
  if (strcmp(name, "Shift_L") == 0 || strcmp(name, "Shift") == 0) return KEY_LEFTSHIFT;
  if (strcmp(name, "Shift_R") == 0) return KEY_RIGHTSHIFT;
  if (strcmp(name, "Control_L") == 0 || strcmp(name, "Control") == 0) return KEY_LEFTCTRL;
  if (strcmp(name, "Control_R") == 0) return KEY_RIGHTCTRL;
  if (strcmp(name, "Alt_L") == 0 || strcmp(name, "Alt") == 0) return KEY_LEFTALT;
  if (strcmp(name, "Alt_R") == 0) return KEY_RIGHTALT;
  if (strcmp(name, "Super_L") == 0 || strcmp(name, "Super") == 0) return KEY_LEFTMETA;
  if (strcmp(name, "Super_R") == 0) return KEY_RIGHTMETA;
  if (strcmp(name, "Meta_L") == 0 || strcmp(name, "Meta") == 0) return KEY_LEFTMETA;
  if (strcmp(name, "Meta_R") == 0) return KEY_RIGHTMETA;
  /* Lock keys */
  if (strcmp(name, "Caps_Lock") == 0) return KEY_CAPSLOCK;
  if (strcmp(name, "Num_Lock") == 0) return KEY_NUMLOCK;
  if (strcmp(name, "Scroll_Lock") == 0) return KEY_SCROLLLOCK;
  /* Function keys */
  if (name[0] == 'F' && isdigit(name[1])) {
    int n = atoi(name + 1);
    if (n >= 1 && n <= 12) return KEY_F1 + (n - 1);
    if (n >= 13 && n <= 24) return KEY_F13 + (n - 13);
  }
  /* Punctuation keysym names */
  if (strcmp(name, "minus") == 0) return KEY_MINUS;
  if (strcmp(name, "equal") == 0) return KEY_EQUAL;
  if (strcmp(name, "bracketleft") == 0) return KEY_LEFTBRACE;
  if (strcmp(name, "bracketright") == 0) return KEY_RIGHTBRACE;
  if (strcmp(name, "semicolon") == 0) return KEY_SEMICOLON;
  if (strcmp(name, "apostrophe") == 0 || strcmp(name, "quoteright") == 0) return KEY_APOSTROPHE;
  if (strcmp(name, "grave") == 0 || strcmp(name, "quoteleft") == 0) return KEY_GRAVE;
  if (strcmp(name, "backslash") == 0) return KEY_BACKSLASH;
  if (strcmp(name, "comma") == 0) return KEY_COMMA;
  if (strcmp(name, "period") == 0) return KEY_DOT;
  if (strcmp(name, "slash") == 0) return KEY_SLASH;
  /* Other */
  if (strcmp(name, "Insert") == 0) return KEY_INSERT;
  if (strcmp(name, "Menu") == 0) return KEY_COMPOSE;
  if (strcmp(name, "Print") == 0) return KEY_SYSRQ;
  if (strcmp(name, "Pause") == 0) return KEY_PAUSE;
  /* Numpad */
  if (strcmp(name, "KP_Multiply") == 0) return KEY_KPASTERISK;
  if (strcmp(name, "KP_Add") == 0) return KEY_KPPLUS;
  if (strcmp(name, "KP_Subtract") == 0) return KEY_KPMINUS;
  if (strcmp(name, "KP_Divide") == 0) return KEY_KPSLASH;
  if (strcmp(name, "KP_Decimal") == 0) return KEY_KPDOT;

  fprintf(stderr, "libxdo_wrapper: unknown keysym '%s'\n", name);
  return -1;
}

static FILE *dbglog = NULL;
static FILE *get_dbglog(void) {
  if (!dbglog) dbglog = fopen("/tmp/libxdo_wrapper.log", "a");
  return dbglog;
}

int xdo_send_keysequence_window_down(void *xdo, unsigned long window, const char *keyseq, int delay) {
  wrapper_xdo_t *w = (wrapper_xdo_t *)xdo;
  if (!w) return 1;
  if (w->use_uinput) {
    FILE *log = get_dbglog();
    if (log) { fprintf(log, "key_down: '%s'\n", keyseq); fflush(log); }
    char buf[256];
    strncpy(buf, keyseq, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *tok = strtok(buf, "+");
    while (tok) {
      int need_shift = 0;
      int code = keysym_name_to_linux(tok, &need_shift);
      if (code >= 0) {
        if (need_shift) { emit(w->kbd_fd, EV_KEY, KEY_LEFTSHIFT, 1); syn(w->kbd_fd); }
        emit(w->kbd_fd, EV_KEY, code, 1);
        syn(w->kbd_fd);
      }
      tok = strtok(NULL, "+");
    }
    return 0;
  }
  ensure_real_libxdo();
  if (!real_libxdo || !w->real_xdo) return 1;
  typedef int (*fn_t)(void *, unsigned long, const char *, int);
  return ((fn_t)dlsym(real_libxdo, "xdo_send_keysequence_window_down"))(w->real_xdo, window, keyseq, delay);
}

int xdo_send_keysequence_window_up(void *xdo, unsigned long window, const char *keyseq, int delay) {
  wrapper_xdo_t *w = (wrapper_xdo_t *)xdo;
  if (!w) return 1;
  if (w->use_uinput) {
    FILE *log = get_dbglog();
    if (log) { fprintf(log, "key_up: '%s'\n", keyseq); fflush(log); }
    char buf[256];
    strncpy(buf, keyseq, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *tok = strtok(buf, "+");
    while (tok) {
      int need_shift = 0;
      int code = keysym_name_to_linux(tok, &need_shift);
      if (code >= 0) {
        emit(w->kbd_fd, EV_KEY, code, 0);
        syn(w->kbd_fd);
        if (need_shift) { emit(w->kbd_fd, EV_KEY, KEY_LEFTSHIFT, 0); syn(w->kbd_fd); }
      }
      tok = strtok(NULL, "+");
    }
    return 0;
  }
  ensure_real_libxdo();
  if (!real_libxdo || !w->real_xdo) return 1;
  typedef int (*fn_t)(void *, unsigned long, const char *, int);
  return ((fn_t)dlsym(real_libxdo, "xdo_send_keysequence_window_up"))(w->real_xdo, window, keyseq, delay);
}

int xdo_send_keysequence_window(void *xdo, unsigned long window, const char *keyseq, int delay) {
  xdo_send_keysequence_window_down(xdo, window, keyseq, delay);
  usleep(delay > 0 ? delay * 1000 : 12000);
  xdo_send_keysequence_window_up(xdo, window, keyseq, delay);
  return 0;
}

int xdo_enter_text_window(void *xdo, unsigned long window, const char *text, int delay) {
  wrapper_xdo_t *w = (wrapper_xdo_t *)xdo;
  if (!w) return 1;
  if (w->use_uinput) {
    for (const char *p = text; *p; p++) {
      int need_shift = 0;
      int code = -1;
      char c = *p;

      if (c >= 'a' && c <= 'z') code = KEY_A + (c - 'a');
      else if (c >= 'A' && c <= 'Z') { code = KEY_A + (c - 'A'); need_shift = 1; }
      else if (c >= '1' && c <= '9') code = KEY_1 + (c - '1');
      else if (c == '0') code = KEY_0;
      else if (c == ' ') code = KEY_SPACE;
      else if (c == '\n' || c == '\r') code = KEY_ENTER;
      else if (c == '\t') code = KEY_TAB;
      else if (c == '-') code = KEY_MINUS;
      else if (c == '=') code = KEY_EQUAL;
      else if (c == '[') code = KEY_LEFTBRACE;
      else if (c == ']') code = KEY_RIGHTBRACE;
      else if (c == ';') code = KEY_SEMICOLON;
      else if (c == '\'') code = KEY_APOSTROPHE;
      else if (c == '`') code = KEY_GRAVE;
      else if (c == '\\') code = KEY_BACKSLASH;
      else if (c == ',') code = KEY_COMMA;
      else if (c == '.') code = KEY_DOT;
      else if (c == '/') code = KEY_SLASH;
      /* Shifted characters */
      else if (c == '!') { code = KEY_1; need_shift = 1; }
      else if (c == '@') { code = KEY_2; need_shift = 1; }
      else if (c == '#') { code = KEY_3; need_shift = 1; }
      else if (c == '$') { code = KEY_4; need_shift = 1; }
      else if (c == '%') { code = KEY_5; need_shift = 1; }
      else if (c == '^') { code = KEY_6; need_shift = 1; }
      else if (c == '&') { code = KEY_7; need_shift = 1; }
      else if (c == '*') { code = KEY_8; need_shift = 1; }
      else if (c == '(') { code = KEY_9; need_shift = 1; }
      else if (c == ')') { code = KEY_0; need_shift = 1; }
      else if (c == '_') { code = KEY_MINUS; need_shift = 1; }
      else if (c == '+') { code = KEY_EQUAL; need_shift = 1; }
      else if (c == '{') { code = KEY_LEFTBRACE; need_shift = 1; }
      else if (c == '}') { code = KEY_RIGHTBRACE; need_shift = 1; }
      else if (c == ':') { code = KEY_SEMICOLON; need_shift = 1; }
      else if (c == '"') { code = KEY_APOSTROPHE; need_shift = 1; }
      else if (c == '~') { code = KEY_GRAVE; need_shift = 1; }
      else if (c == '|') { code = KEY_BACKSLASH; need_shift = 1; }
      else if (c == '<') { code = KEY_COMMA; need_shift = 1; }
      else if (c == '>') { code = KEY_DOT; need_shift = 1; }
      else if (c == '?') { code = KEY_SLASH; need_shift = 1; }

      if (code >= 0) {
        if (need_shift) { emit(w->kbd_fd, EV_KEY, KEY_LEFTSHIFT, 1); syn(w->kbd_fd); }
        emit(w->kbd_fd, EV_KEY, code, 1); syn(w->kbd_fd);
        usleep(delay > 0 ? delay * 1000 : 5000);
        emit(w->kbd_fd, EV_KEY, code, 0); syn(w->kbd_fd);
        if (need_shift) { emit(w->kbd_fd, EV_KEY, KEY_LEFTSHIFT, 0); syn(w->kbd_fd); }
      }
    }
    return 0;
  }
  ensure_real_libxdo();
  if (!real_libxdo || !w->real_xdo) return 1;
  typedef int (*fn_t)(void *, unsigned long, const char *, int);
  return ((fn_t)dlsym(real_libxdo, "xdo_enter_text_window"))(w->real_xdo, window, text, delay);
}

/* Stubs for other functions RustDesk calls */
int xdo_move_mouse_relative_to_window(void *xdo, unsigned long w, int x, int y) { return 0; }
int xdo_get_mouse_location(void *xdo, int *x, int *y, int *s) { if(x)*x=0; if(y)*y=0; if(s)*s=0; return 0; }
int xdo_get_mouse_location2(void *xdo, int *x, int *y, int *s, unsigned long *w) { if(x)*x=0; if(y)*y=0; if(s)*s=0; if(w)*w=0; return 0; }
int xdo_get_active_window(void *xdo, unsigned long *w) { if(w)*w=0; return 0; }
int xdo_get_focused_window(void *xdo, unsigned long *w) { if(w)*w=0; return 0; }
int xdo_get_focused_window_sane(void *xdo, unsigned long *w) { if(w)*w=0; return 0; }
int xdo_get_window_location(void *xdo, unsigned long w, int *x, int *y, void *s) { if(x)*x=0; if(y)*y=0; return 0; }
int xdo_get_window_size(void *xdo, unsigned long w, unsigned int *ww, unsigned int *hh) { if(ww)*ww=SCREEN_W; if(hh)*hh=SCREEN_H; return 0; }
unsigned int xdo_get_input_state(void *xdo) { return 0; }
int xdo_activate_window(void *xdo, unsigned long w) { return 0; }
int xdo_wait_for_mouse_move_from(void *xdo, int x, int y) { return 0; }
int xdo_wait_for_mouse_move_to(void *xdo, int x, int y) { return 0; }
int xdo_set_window_class(void *xdo, unsigned long w, const char *n, const char *c) { return 0; }
int xdo_search_windows(void *xdo, void *search, unsigned long **wl, unsigned int *nw) { if(wl)*wl=NULL; if(nw)*nw=0; return 0; }
