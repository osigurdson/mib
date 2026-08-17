# Linux convenience build. CMake also builds the native Windows backend.
CC ?= cc
PYTHON ?= python3
PKG_CONFIG ?= pkg-config
WAYLAND_SCANNER ?= wayland-scanner
CMAKE ?= cmake
WINE ?= wine
WINDOWS_BUILD_DIR ?= .build/windows
WINEPREFIX ?= $(abspath .build/wine-prefix)
WINEARCH ?= win64
WINEDEBUG ?= -all
ARGS ?=
PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share
APPLICATIONSDIR ?= $(DATADIR)/applications
ICONDIR ?= $(DATADIR)/icons
DESTDIR ?=
INSTALL ?= install
CFLAGS ?= -Os -g
CFLAGS += -ffunction-sections -fdata-sections -std=c11 -Wall -Wextra -Wpedantic
LDFLAGS += -Wl,--as-needed -Wl,--gc-sections
LINUX_BUILD_DIR ?= .build/linux
TEST_BIN := $(LINUX_BUILD_DIR)/tests
TOOL_BIN := $(LINUX_BUILD_DIR)/tools
GEN := $(LINUX_BUILD_DIR)/generated
INCLUDES := -Isrc -I$(GEN)
WAYLAND_CFLAGS := $(shell $(PKG_CONFIG) --cflags wayland-client xkbcommon)
WAYLAND_LIBS := $(shell $(PKG_CONFIG) --libs wayland-client xkbcommon)
PROTOCOLS_DIR := $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)
PROTOCOL_NAMES := xdg-shell viewporter fractional-scale-v1 xdg-activation-v1 xdg-decoration-unstable-v1
PROTOCOL_HEADERS := $(addprefix $(GEN)/,$(addsuffix -client-protocol.h,$(PROTOCOL_NAMES)))
PROTOCOL_SOURCES := $(addprefix $(GEN)/,$(addsuffix -protocol.c,$(PROTOCOL_NAMES)))
CORE := src/app.c src/session.c src/media.c src/image.c src/editor.c src/keys.c src/ex.c src/history.c src/substitute.c src/markdown.c src/render.c src/tiny-font.c
HEADERS := $(wildcard src/*.h src/platform/*/*.inc)
MEMORY := src/platform/linux/memory.c
FILES := src/platform/linux/files.c
WINDOW := src/platform/linux/wayland.c
FONT_DATA := $(GEN)/font-data.h

.PHONY: all run install windows run-windows test-windows clean test memory subset-font jot editor-test client-test font-test app-test modal-test session-test media-test ocr-test clipboard-test page-preview font-preview
all: jot

$(LINUX_BUILD_DIR) $(TEST_BIN) $(TOOL_BIN) $(GEN):
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

$(GEN)/xdg-activation-v1-client-protocol.h: $(PROTOCOLS_DIR)/staging/xdg-activation/xdg-activation-v1.xml | $(GEN)
	$(WAYLAND_SCANNER) client-header $< $@
$(GEN)/xdg-activation-v1-protocol.c: $(PROTOCOLS_DIR)/staging/xdg-activation/xdg-activation-v1.xml | $(GEN)
	$(WAYLAND_SCANNER) private-code $< $@

$(GEN)/xdg-decoration-unstable-v1-client-protocol.h: $(PROTOCOLS_DIR)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml | $(GEN)
	$(WAYLAND_SCANNER) client-header $< $@
$(GEN)/xdg-decoration-unstable-v1-protocol.c: $(PROTOCOLS_DIR)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml | $(GEN)
	$(WAYLAND_SCANNER) private-code $< $@

$(FONT_DATA): fonts/editor.tvf tools/embed-font.py | $(GEN)
	$(PYTHON) tools/embed-font.py $< $@

jot: $(LINUX_BUILD_DIR)/jot

$(LINUX_BUILD_DIR)/jot: src/main.c src/platform/linux/entry.c $(CORE) $(HEADERS) $(MEMORY) $(FILES) $(WINDOW) $(FONT_DATA) $(PROTOCOL_HEADERS) $(PROTOCOL_SOURCES) | $(LINUX_BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) $(WAYLAND_CFLAGS) $(LDFLAGS) -o $@ src/main.c src/platform/linux/entry.c $(CORE) $(MEMORY) $(FILES) $(WINDOW) $(PROTOCOL_SOURCES) $(WAYLAND_LIBS) $(LDLIBS)

run: jot
	"$(LINUX_BUILD_DIR)/jot" $(ARGS)

install: jot
	$(INSTALL) -d "$(DESTDIR)$(BINDIR)" "$(DESTDIR)$(APPLICATIONSDIR)" "$(DESTDIR)$(ICONDIR)/hicolor/scalable/apps"
	$(INSTALL) -m 755 "$(LINUX_BUILD_DIR)/jot" "$(DESTDIR)$(BINDIR)/jot"
	$(INSTALL) -m 644 packaging/dev.jot.Jot.svg "$(DESTDIR)$(ICONDIR)/hicolor/scalable/apps/dev.jot.Jot.svg"
	$(PYTHON) tools/install-desktop.py "$(BINDIR)/jot" "$(DESTDIR)$(APPLICATIONSDIR)/dev.jot.Jot.desktop"

windows:
	$(CMAKE) -S . -B "$(WINDOWS_BUILD_DIR)" -G Ninja \
		-DCMAKE_TOOLCHAIN_FILE=tools/cmake/zig-windows.cmake \
		-DCMAKE_BUILD_TYPE=MinSizeRel -DBUILD_TESTING=ON
	$(CMAKE) --build "$(WINDOWS_BUILD_DIR)" --parallel 4

run-windows: windows
	WINEPREFIX="$(WINEPREFIX)" WINEARCH="$(WINEARCH)" WINEDEBUG="$(WINEDEBUG)" \
		$(WINE) "$(WINDOWS_BUILD_DIR)/jot.exe" $(ARGS)

test-windows: windows
	WINEPREFIX="$(WINEPREFIX)" WINEARCH="$(WINEARCH)" WINEDEBUG="$(WINEDEBUG)" \
		./tools/test-wine.sh "$(abspath $(WINDOWS_BUILD_DIR))"

editor-test: $(TEST_BIN)/editor-test

$(TEST_BIN)/editor-test: tests/editor-test.c src/editor.c src/keys.c src/ex.c src/history.c src/substitute.c $(HEADERS) $(FILES) | $(TEST_BIN)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) $(LDFLAGS) -o $@ tests/editor-test.c src/editor.c src/keys.c src/ex.c src/history.c src/substitute.c $(FILES) $(LDLIBS)

client-test: $(TEST_BIN)/client-test

$(TEST_BIN)/client-test: tests/client-test.c $(CORE) $(HEADERS) $(MEMORY) $(FILES) $(WINDOW) $(FONT_DATA) $(PROTOCOL_HEADERS) $(PROTOCOL_SOURCES) | $(TEST_BIN)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) $(WAYLAND_CFLAGS) $(LDFLAGS) -o $@ tests/client-test.c $(CORE) $(MEMORY) $(FILES) $(PROTOCOL_SOURCES) $(WAYLAND_LIBS) $(LDLIBS)

font-test: $(TEST_BIN)/font-test

$(TEST_BIN)/font-test: tests/font-test.c src/tiny-font.c $(HEADERS) $(FONT_DATA) $(MEMORY) | $(TEST_BIN)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) $(LDFLAGS) -o $@ tests/font-test.c $(MEMORY) $(LDLIBS)

app-test: $(TEST_BIN)/app-test

$(TEST_BIN)/app-test: tests/app-test.c $(CORE) $(HEADERS) $(FONT_DATA) $(MEMORY) $(FILES) | $(TEST_BIN)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) $(LDFLAGS) -o $@ tests/app-test.c $(CORE) $(MEMORY) $(FILES) $(LDLIBS)

modal-test: $(TEST_BIN)/modal-test

$(TEST_BIN)/modal-test: tests/modal-test.c src/editor.c src/keys.c src/ex.c src/history.c src/substitute.c $(HEADERS) $(FILES) | $(TEST_BIN)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) $(LDFLAGS) -o $@ tests/modal-test.c src/editor.c src/keys.c src/ex.c src/history.c src/substitute.c $(FILES) $(LDLIBS)

session-test: $(TEST_BIN)/session-test

$(TEST_BIN)/session-test: tests/session-test.c $(CORE) $(HEADERS) $(FONT_DATA) $(MEMORY) $(FILES) | $(TEST_BIN)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) $(LDFLAGS) -o $@ tests/session-test.c $(CORE) $(MEMORY) $(FILES) $(LDLIBS)

media-test: $(TEST_BIN)/media-test

$(TEST_BIN)/media-test: tests/media-test.c $(CORE) $(HEADERS) $(FONT_DATA) $(MEMORY) $(FILES) | $(TEST_BIN)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) $(LDFLAGS) -o $@ tests/media-test.c $(CORE) $(MEMORY) $(FILES) $(LDLIBS)

ocr-test: $(TEST_BIN)/ocr-test

$(TEST_BIN)/ocr-test: tests/ocr-test.c $(CORE) $(HEADERS) $(FONT_DATA) $(MEMORY) $(FILES) | $(TEST_BIN)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) $(LDFLAGS) -o $@ tests/ocr-test.c $(CORE) $(MEMORY) $(FILES) $(LDLIBS)

clipboard-test: $(TEST_BIN)/clipboard-test

$(TEST_BIN)/clipboard-test: tests/clipboard-test.c $(CORE) $(HEADERS) $(MEMORY) $(FILES) $(WINDOW) $(FONT_DATA) $(PROTOCOL_HEADERS) $(PROTOCOL_SOURCES) | $(TEST_BIN)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) $(WAYLAND_CFLAGS) $(LDFLAGS) -o $@ tests/clipboard-test.c $(CORE) $(MEMORY) $(FILES) $(PROTOCOL_SOURCES) $(WAYLAND_LIBS) $(shell $(PKG_CONFIG) --libs wayland-server) $(LDLIBS)

OPTIONAL_OCR_TEST := $(if $(shell command -v tesseract 2>/dev/null),ocr-test)

test: editor-test client-test font-test app-test modal-test session-test media-test clipboard-test $(OPTIONAL_OCR_TEST)
	$(if $(OPTIONAL_OCR_TEST),cd "$(TEST_BIN)" && ./ocr-test,true)
	cd "$(TEST_BIN)" && ./clipboard-test
	cd "$(TEST_BIN)" && ./media-test
	cd "$(TEST_BIN)" && ./session-test
	cd "$(TEST_BIN)" && ./modal-test
	cd "$(TEST_BIN)" && ./editor-test
	cd "$(TEST_BIN)" && ./client-test
	cd "$(TEST_BIN)" && ./font-test
	cd "$(TEST_BIN)" && ./app-test

page-preview: $(TOOL_BIN)/page-preview

$(TOOL_BIN)/page-preview: tools/page-preview.c $(CORE) $(HEADERS) $(FONT_DATA) $(MEMORY) $(FILES) | $(TOOL_BIN)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) $(LDFLAGS) -o $@ tools/page-preview.c $(CORE) $(MEMORY) $(FILES) $(LDLIBS)

font-preview: $(TOOL_BIN)/font-preview

$(TOOL_BIN)/font-preview: tools/font-preview.c src/editor.c src/keys.c src/ex.c src/history.c src/substitute.c src/tiny-font.c $(HEADERS) $(FONT_DATA) $(MEMORY) $(FILES) | $(TOOL_BIN)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) $(LDFLAGS) -o $@ tools/font-preview.c src/editor.c src/keys.c src/ex.c src/history.c src/substitute.c src/tiny-font.c $(MEMORY) $(FILES) $(LDLIBS)

subset-font: | $(GEN)
	$(PYTHON) tools/subset-font.py fonts/JetBrainsMonoNerdFontMono-Regular.ttf fonts/editor.tvf
	$(PYTHON) tools/embed-font.py fonts/editor.tvf $(FONT_DATA)

memory: jot
	$(PYTHON) tools/measure-memory.py --binary "$(LINUX_BUILD_DIR)/jot"

clean:
	rm -rf "$(LINUX_BUILD_DIR)"
