CC ?= cc
PKG_CONFIG ?= pkg-config
WAYLAND_SCANNER ?= wayland-scanner
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
WAYLAND_CFLAGS := $(shell $(PKG_CONFIG) --cflags wayland-client freetype2 fontconfig xkbcommon)
WAYLAND_LIBS := $(shell $(PKG_CONFIG) --libs wayland-client freetype2 fontconfig xkbcommon)
PROTOCOLS_DIR := $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)
PROTOCOL_NAMES := xdg-shell viewporter fractional-scale-v1
PROTOCOL_HEADERS := $(addsuffix -client-protocol.h,$(PROTOCOL_NAMES))
PROTOCOL_SOURCES := $(addsuffix -protocol.c,$(PROTOCOL_NAMES))
XDG_SHELL := $(PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml

.PHONY: all run clean test
all: tiny-vi

xdg-shell-client-protocol.h: $(XDG_SHELL)
	$(WAYLAND_SCANNER) client-header $< $@

xdg-shell-protocol.c: $(XDG_SHELL)
	$(WAYLAND_SCANNER) private-code $< $@

viewporter-client-protocol.h: $(PROTOCOLS_DIR)/stable/viewporter/viewporter.xml
	$(WAYLAND_SCANNER) client-header $< $@

viewporter-protocol.c: $(PROTOCOLS_DIR)/stable/viewporter/viewporter.xml
	$(WAYLAND_SCANNER) private-code $< $@

fractional-scale-v1-client-protocol.h: $(PROTOCOLS_DIR)/staging/fractional-scale/fractional-scale-v1.xml
	$(WAYLAND_SCANNER) client-header $< $@

fractional-scale-v1-protocol.c: $(PROTOCOLS_DIR)/staging/fractional-scale/fractional-scale-v1.xml
	$(WAYLAND_SCANNER) private-code $< $@

tiny-vi: hello.c editor.c editor.h $(PROTOCOL_HEADERS) $(PROTOCOL_SOURCES)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WAYLAND_CFLAGS) $(LDFLAGS) -o $@ hello.c editor.c $(PROTOCOL_SOURCES) $(WAYLAND_LIBS) $(LDLIBS)

run: tiny-vi
	./tiny-vi

clean:
	rm -f tiny-vi hello-wayland editor-test client-test $(PROTOCOL_HEADERS) $(PROTOCOL_SOURCES)

editor-test: tests/editor-test.c editor.c editor.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. $(LDFLAGS) -o $@ tests/editor-test.c editor.c $(LDLIBS)

client-test: tests/client-test.c hello.c editor.c editor.h $(PROTOCOL_HEADERS) $(PROTOCOL_SOURCES)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WAYLAND_CFLAGS) -I. $(LDFLAGS) -o $@ tests/client-test.c editor.c $(PROTOCOL_SOURCES) $(WAYLAND_LIBS) $(LDLIBS)

test: editor-test client-test
	./editor-test
	./client-test
