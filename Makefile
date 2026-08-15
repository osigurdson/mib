# Linux convenience build. CMake also configures the Windows stub backend.
CC ?= cc
PYTHON ?= python3
PKG_CONFIG ?= pkg-config
WAYLAND_SCANNER ?= wayland-scanner
CFLAGS ?= -Os -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
LDFLAGS += -Wl,--as-needed
GEN := .build/generated
INCLUDES := -Isrc -I$(GEN)
WAYLAND_CFLAGS := $(shell $(PKG_CONFIG) --cflags wayland-client xkbcommon)
WAYLAND_LIBS := $(shell $(PKG_CONFIG) --libs wayland-client xkbcommon)
PROTOCOLS_DIR := $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)
PROTOCOL_NAMES := xdg-shell viewporter fractional-scale-v1
PROTOCOL_HEADERS := $(addprefix $(GEN)/,$(addsuffix -client-protocol.h,$(PROTOCOL_NAMES)))
PROTOCOL_SOURCES := $(addprefix $(GEN)/,$(addsuffix -protocol.c,$(PROTOCOL_NAMES)))
CORE := src/app.c src/editor.c src/render.c src/tiny-font.c
HEADERS := $(wildcard src/*.h)
MEMORY := src/platform/linux/memory.c
FILES := src/platform/linux/files.c
WINDOW := src/platform/linux/wayland.c
FONT_DATA := $(GEN)/font-data.h

.PHONY: all run clean test memory subset-font
all: tiny-vi

$(GEN):
	mkdir -p $@

$(GEN)/xdg-shell-client-protocol.h: $(PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml | $(GEN)
	$(WAYLAND_SCANNER) client-header $< $@
$(GEN)/xdg-shell-protocol.c: $(PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml | $(GEN)
	$(WAYLAND_SCANNER) private-code $< $@
$(GEN)/viewporter-client-protocol.h: $(PROTOCOLS_DIR)/stable/viewporter/viewporter.xml | $(GEN)
	$(WAYLAND_SCANNER) client-header $< $@
$(GEN)/viewporter-protocol.c: $(PROTOCOLS_DIR)/stable/viewporter/viewporter.xml | $(GEN)
	$(WAYLAND_SCANNER) private-code $< $@
$(GEN)/fractional-scale-v1-client-protocol.h: $(PROTOCOLS_DIR)/staging/fractional-scale/fractional-scale-v1.xml | $(GEN)
	$(WAYLAND_SCANNER) client-header $< $@
$(GEN)/fractional-scale-v1-protocol.c: $(PROTOCOLS_DIR)/staging/fractional-scale/fractional-scale-v1.xml | $(GEN)
	$(WAYLAND_SCANNER) private-code $< $@

$(FONT_DATA): fonts/editor.tvf tools/embed-font.py | $(GEN)
	$(PYTHON) tools/embed-font.py $< $@

tiny-vi: src/main.c $(CORE) $(HEADERS) $(MEMORY) $(FILES) $(WINDOW) $(FONT_DATA) $(PROTOCOL_HEADERS) $(PROTOCOL_SOURCES)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) $(WAYLAND_CFLAGS) $(LDFLAGS) -o $@ src/main.c $(CORE) $(MEMORY) $(FILES) $(WINDOW) $(PROTOCOL_SOURCES) $(WAYLAND_LIBS) $(LDLIBS)

run: tiny-vi
	./tiny-vi

editor-test: tests/editor-test.c src/editor.c $(HEADERS) $(FILES)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) $(LDFLAGS) -o $@ tests/editor-test.c src/editor.c $(FILES) $(LDLIBS)

client-test: tests/client-test.c $(CORE) $(HEADERS) $(MEMORY) $(FILES) $(WINDOW) $(FONT_DATA) $(PROTOCOL_HEADERS) $(PROTOCOL_SOURCES)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) $(WAYLAND_CFLAGS) $(LDFLAGS) -o $@ tests/client-test.c $(CORE) $(MEMORY) $(FILES) $(PROTOCOL_SOURCES) $(WAYLAND_LIBS) $(LDLIBS)

font-test: tests/font-test.c src/tiny-font.c $(HEADERS) $(FONT_DATA) $(MEMORY)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) $(LDFLAGS) -o $@ tests/font-test.c $(MEMORY) $(LDLIBS)

app-test: tests/app-test.c $(CORE) $(HEADERS) $(FONT_DATA) $(MEMORY) $(FILES)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) $(LDFLAGS) -o $@ tests/app-test.c $(CORE) $(MEMORY) $(FILES) $(LDLIBS)

platform-stub-test: tests/platform-stub-test.c $(CORE) $(HEADERS) $(FONT_DATA) $(MEMORY) src/platform/windows/files_stub.c src/platform/windows/window_stub.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) $(LDFLAGS) -o $@ tests/platform-stub-test.c $(CORE) $(MEMORY) src/platform/windows/files_stub.c src/platform/windows/window_stub.c $(LDLIBS)

test: editor-test client-test font-test app-test platform-stub-test
	./editor-test
	./client-test
	./font-test
	./app-test
	./platform-stub-test

font-preview: tools/font-preview.c src/editor.c src/tiny-font.c $(HEADERS) $(FONT_DATA) $(MEMORY) $(FILES)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) $(LDFLAGS) -o $@ tools/font-preview.c src/editor.c src/tiny-font.c $(MEMORY) $(FILES) $(LDLIBS)

subset-font: | $(GEN)
	$(PYTHON) tools/subset-font.py fonts/JetBrainsMonoNerdFontMono-Regular.ttf fonts/editor.tvf
	$(PYTHON) tools/embed-font.py fonts/editor.tvf $(FONT_DATA)

memory: tiny-vi
	$(PYTHON) tools/measure-memory.py

clean:
	rm -f tiny-vi editor-test client-test font-test app-test platform-stub-test font-preview
	rm -rf $(GEN)
