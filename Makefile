PREFIX ?= $(HOME)/.local
LIBDIR = $(PREFIX)/lib
BINDIR = $(PREFIX)/bin

CC = gcc
CFLAGS = -shared -fPIC -O2

.PHONY: all install uninstall clean

all: build/rustdesk_display_override.so build/libxdo_wrapper.so

build/rustdesk_display_override.so: src/rustdesk_display_override.c | build
	$(CC) $(CFLAGS) -o $@ $< -ldl

build/libxdo_wrapper.so: src/libxdo_wrapper.c | build
	$(CC) $(CFLAGS) -o $@ $< -ldl

build:
	mkdir -p build

install: all
	install -Dm755 build/rustdesk_display_override.so $(LIBDIR)/rustdesk_display_override.so
	install -Dm755 build/libxdo_wrapper.so $(LIBDIR)/libxdo_wrapper.so
	install -Dm755 scripts/portal-screencast.py $(BINDIR)/portal-screencast.py
	install -Dm755 scripts/rustdesk-wayland.sh $(BINDIR)/rustdesk-wayland.sh
	install -Dm644 scripts/99-uinput.rules $(LIBDIR)/rustdesk-wayland/99-uinput.rules
	@echo ""
	@echo "Installed. Now run the setup:"
	@echo "  rustdesk-wayland.sh install"
	@echo "  rustdesk-wayland.sh start"

uninstall:
	rm -f $(LIBDIR)/rustdesk_display_override.so
	rm -f $(LIBDIR)/libxdo_wrapper.so
	rm -f $(BINDIR)/portal-screencast.py
	rm -f $(BINDIR)/rustdesk-wayland.sh
	rm -rf $(LIBDIR)/rustdesk-wayland

clean:
	rm -rf build
