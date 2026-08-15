CC ?= cc
PKG_CONFIG ?= pkg-config
WAYLAND_SCANNER ?= wayland-scanner
CFLAGS ?= -Os -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
LDFLAGS += -Wl,--as-needed
WAYLAND_CFLAGS := $(shell $(PKG_CONFIG) --cflags wayland-client xkbcommon)
WAYLAND_LIBS := $(shell $(PKG_CONFIG) --libs wayland-client xkbcommon)
PROTOCOLS_DIR := $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)
PROTOCOL_NAMES := xdg-shell viewporter fractional-scale-v1
PROTOCOL_HEADERS := $(addsuffix -client-protocol.h,$(PROTOCOL_NAMES))
PROTOCOL_SOURCES := $(addsuffix -protocol.c,$(PROTOCOL_NAMES))
XDG_SHELL := $(PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml

.PHONY: all run clean test memory
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

tiny-vi: hello.c editor.c editor.h tiny-font.c tiny-font.h font-data.h $(PROTOCOL_HEADERS) $(PROTOCOL_SOURCES)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WAYLAND_CFLAGS) $(LDFLAGS) -o $@ hello.c editor.c tiny-font.c $(PROTOCOL_SOURCES) $(WAYLAND_LIBS) $(LDLIBS)

run: tiny-vi
	./tiny-vi

clean:
	rm -f tiny-vi hello-wayland editor-test client-test font-data.h font-test font-preview $(PROTOCOL_HEADERS) $(PROTOCOL_SOURCES)

editor-test: tests/editor-test.c editor.c editor.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. $(LDFLAGS) -o $@ tests/editor-test.c editor.c $(LDLIBS)

client-test: tests/client-test.c hello.c editor.c editor.h tiny-font.c tiny-font.h font-data.h $(PROTOCOL_HEADERS) $(PROTOCOL_SOURCES)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WAYLAND_CFLAGS) -I. $(LDFLAGS) -o $@ tests/client-test.c editor.c tiny-font.c $(PROTOCOL_SOURCES) $(WAYLAND_LIBS) $(LDLIBS)

test: editor-test client-test font-test
	./editor-test
	./client-test
	./font-test

font-data.h: fonts/editor.tvf tools/embed-font.py
	python3 tools/embed-font.py $< $@

.PHONY: subset-font
subset-font:
	python3 tools/subset-font.py fonts/JetBrainsMonoNerdFontMono-Regular.ttf fonts/editor.tvf
	python3 tools/embed-font.py fonts/editor.tvf font-data.h

memory: tiny-vi
	python3 tools/measure-memory.py

font-test: tests/font-test.c tiny-font.c tiny-font.h font-data.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. $(LDFLAGS) -o $@ tests/font-test.c $(LDLIBS)

font-preview: tools/font-preview.c tiny-font.c tiny-font.h font-data.h editor.c editor.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -I. $(LDFLAGS) -o $@ tools/font-preview.c tiny-font.c editor.c $(LDLIBS)
