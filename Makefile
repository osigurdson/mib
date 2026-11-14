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
# Installed binaries drop debug info; pass INSTALL_STRIP= to keep it.
INSTALL_STRIP ?= -s
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
PROTOCOL_NAMES := xdg-shell viewporter fractional-scale-v1 xdg-decoration-unstable-v1 cursor-shape-v1 tablet-unstable-v2
PROTOCOL_HEADERS := $(addprefix $(GEN)/,$(addsuffix -client-protocol.h,$(PROTOCOL_NAMES)))
PROTOCOL_SOURCES := $(addprefix $(GEN)/,$(addsuffix -protocol.c,$(PROTOCOL_NAMES)))
CORE := src/app.c src/session.c src/pages.c src/media.c src/image.c src/editor.c src/keys.c src/ex.c src/history.c src/substitute.c src/markdown.c src/render.c src/navigator.c src/help.c src/tiny-font.c
# version.h is generated: src/ex.c reports it for :version.
HEADERS := $(wildcard src/*.h src/platform/*/*.inc) $(GEN)/version.h
MEMORY := src/platform/linux/memory.c
FILES := src/platform/linux/files.c
WINDOW := src/platform/linux/wayland.c
FONT_DATA := $(GEN)/font-data.h
CLANG_FORMAT ?= clang-format
# Every C file mib owns; third_party/ keeps its upstream formatting.
FORMAT_FILES = $(shell git ls-files '*.c' '*.h' '*.inc' ':!third_party')

.PHONY: FORCE all compile-commands format format-check fuzz run install windows run-windows test-windows test-linux-e2e vim-compare clean test memory subset-font mib editor-test client-test font-test app-test modal-test session-test media-test ocr-test clipboard-test image-test page-preview font-preview demo-frames demo
all: mib

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


$(GEN)/xdg-decoration-unstable-v1-client-protocol.h: $(PROTOCOLS_DIR)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml | $(GEN)
	$(WAYLAND_SCANNER) client-header $< $@
$(GEN)/xdg-decoration-unstable-v1-protocol.c: $(PROTOCOLS_DIR)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml | $(GEN)
	$(WAYLAND_SCANNER) private-code $< $@

$(GEN)/cursor-shape-v1-client-protocol.h: $(PROTOCOLS_DIR)/staging/cursor-shape/cursor-shape-v1.xml | $(GEN)
	$(WAYLAND_SCANNER) client-header $< $@
$(GEN)/cursor-shape-v1-protocol.c: $(PROTOCOLS_DIR)/staging/cursor-shape/cursor-shape-v1.xml | $(GEN)
	$(WAYLAND_SCANNER) private-code $< $@
# Cursor-shape references the tablet-tool interface, even when only pointers are used.
$(GEN)/tablet-unstable-v2-client-protocol.h: $(PROTOCOLS_DIR)/unstable/tablet/tablet-unstable-v2.xml | $(GEN)
	$(WAYLAND_SCANNER) client-header $< $@
$(GEN)/tablet-unstable-v2-protocol.c: $(PROTOCOLS_DIR)/unstable/tablet/tablet-unstable-v2.xml | $(GEN)
	$(WAYLAND_SCANNER) private-code $< $@

$(FONT_DATA): fonts/editor.tvf tools/embed-font.py | $(GEN)
	$(PYTHON) tools/embed-font.py $< $@

# Checked on every build; tools/version.py only rewrites it when the version or commit changes.
$(GEN)/version.h: FORCE | $(GEN)
	@$(PYTHON) tools/version.py $@

FORCE:

mib: $(LINUX_BUILD_DIR)/mib

$(LINUX_BUILD_DIR)/mib: src/main.c src/platform/linux/entry.c $(CORE) $(HEADERS) $(MEMORY) $(FILES) $(WINDOW) $(FONT_DATA) $(PROTOCOL_HEADERS) $(PROTOCOL_SOURCES) | $(LINUX_BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) $(WAYLAND_CFLAGS) $(LDFLAGS) -o $@ src/main.c src/platform/linux/entry.c $(CORE) $(MEMORY) $(FILES) $(WINDOW) $(PROTOCOL_SOURCES) $(WAYLAND_LIBS) $(LDLIBS)

run: mib
	"$(LINUX_BUILD_DIR)/mib"

install: mib
	$(INSTALL) -d "$(DESTDIR)$(BINDIR)" "$(DESTDIR)$(APPLICATIONSDIR)" "$(DESTDIR)$(ICONDIR)/hicolor/scalable/apps"
	$(INSTALL) $(INSTALL_STRIP) -m 755 "$(LINUX_BUILD_DIR)/mib" "$(DESTDIR)$(BINDIR)/mib"
	$(INSTALL) -m 644 packaging/io.github.osigurdson.mib.svg "$(DESTDIR)$(ICONDIR)/hicolor/scalable/apps/io.github.osigurdson.mib.svg"
	$(PYTHON) tools/install-desktop.py "$(BINDIR)/mib" "$(DESTDIR)$(APPLICATIONSDIR)/io.github.osigurdson.mib.desktop"

windows:
	$(CMAKE) -S . -B "$(WINDOWS_BUILD_DIR)" -G Ninja \
		-DCMAKE_TOOLCHAIN_FILE=tools/cmake/zig-windows.cmake \
		-DCMAKE_BUILD_TYPE=MinSizeRel -DBUILD_TESTING=ON
	$(CMAKE) --build "$(WINDOWS_BUILD_DIR)" --parallel 4

run-windows: windows
	WINEPREFIX="$(WINEPREFIX)" WINEARCH="$(WINEARCH)" WINEDEBUG="$(WINEDEBUG)" \
		$(WINE) "$(WINDOWS_BUILD_DIR)/mib.exe"

test-windows: windows
	WINEPREFIX="$(WINEPREFIX)" WINEARCH="$(WINEARCH)" WINEDEBUG="$(WINEDEBUG)" \
		./tools/test-wine.sh "$(abspath $(WINDOWS_BUILD_DIR))"

editor-test: $(TEST_BIN)/editor-test

$(TEST_BIN)/editor-test: tests/editor-test.c src/editor.c src/keys.c src/ex.c src/history.c src/substitute.c $(HEADERS) $(FILES) | $(TEST_BIN)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) $(LDFLAGS) -o $@ tests/editor-test.c src/editor.c src/keys.c src/ex.c src/history.c src/substitute.c $(FILES) $(LDLIBS)

client-test: $(TEST_BIN)/client-test

$(TEST_BIN)/client-test: tests/client-test.c $(CORE) $(HEADERS) $(MEMORY) $(FILES) $(WINDOW) $(FONT_DATA) $(PROTOCOL_HEADERS) $(PROTOCOL_SOURCES) | $(TEST_BIN)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) $(WAYLAND_CFLAGS) $(LDFLAGS) -o $@ tests/client-test.c $(CORE) $(MEMORY) $(FILES) $(PROTOCOL_SOURCES) $(WAYLAND_LIBS) $(shell $(PKG_CONFIG) --libs wayland-server) $(LDLIBS)

image-test: $(TEST_BIN)/image-test

# Assertions off and AddressSanitizer on: the build in which an encoder that ignores a failed
# allocation would write out of bounds.
IMAGE_TEST_FLAGS ?= -DNDEBUG -fsanitize=address,undefined -fno-sanitize-recover=all

$(TEST_BIN)/image-test: tests/image-test.c src/image.c $(HEADERS) | $(TEST_BIN)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(IMAGE_TEST_FLAGS) $(INCLUDES) $(LDFLAGS) -o $@ tests/image-test.c $(LDLIBS)

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

# Tests keep mib's shared settings file away from the user's own ~/.config/mib.
test: export XDG_CONFIG_HOME = $(abspath $(TEST_BIN))/config
test: export XDG_STATE_HOME = $(abspath $(TEST_BIN))/state
test: editor-test client-test font-test app-test modal-test session-test media-test clipboard-test image-test $(OPTIONAL_OCR_TEST)
	rm -rf "$(abspath $(TEST_BIN))/config" "$(abspath $(TEST_BIN))/state"
	$(if $(OPTIONAL_OCR_TEST),cd "$(TEST_BIN)" && ./ocr-test,true)
	cd "$(TEST_BIN)" && ./image-test
	cd "$(TEST_BIN)" && ./clipboard-test
	cd "$(TEST_BIN)" && ./media-test
	cd "$(TEST_BIN)" && ./session-test
	cd "$(TEST_BIN)" && ./modal-test
	cd "$(TEST_BIN)" && ./editor-test
	cd "$(TEST_BIN)" && ./client-test
	cd "$(TEST_BIN)" && ./font-test
	cd "$(TEST_BIN)" && ./app-test

# Optional fuzzing of image files, page text in every view and pasted fragments (libFuzzer,
# so clang). Runs for FUZZ_TIME seconds, keeping its corpus in the build folder; crashes are
# written there as crash-*. ARGS passes more libFuzzer options.
FUZZ_CC ?= clang
FUZZ_TIME ?= 60
FUZZ_DIR := $(LINUX_BUILD_DIR)/fuzz

$(FUZZ_DIR) $(FUZZ_DIR)/corpus:
	mkdir -p $@

$(FUZZ_DIR)/mib-fuzz: tests/fuzz.c $(CORE) $(HEADERS) $(FONT_DATA) $(MEMORY) $(FILES) | $(FUZZ_DIR)
	$(FUZZ_CC) -g -O1 -std=c11 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined $(INCLUDES) -o $@ tests/fuzz.c $(CORE) $(MEMORY) $(FILES) -lm

# The first byte of an input picks its target, so each seed starts with it: 0 image, 1 page
# text, 2 fragment. ImageMagick, when present, adds a JPEG seed.
$(FUZZ_DIR)/corpus/seeded: tests/fuzz-seed.png README.md | $(FUZZ_DIR)/corpus
	{ printf '\000'; cat tests/fuzz-seed.png; } > $(FUZZ_DIR)/corpus/seed-png
	if command -v magick >/dev/null; then { printf '\000'; magick tests/fuzz-seed.png -resize 96x jpg:-; } > $(FUZZ_DIR)/corpus/seed-jpeg; fi
	{ printf '\001'; cat README.md; } > $(FUZZ_DIR)/corpus/seed-page
	printf '\002MIBFRG1\000\004\000\000\000\022\000\000\000/tmp![](assets/x.png)\n' > $(FUZZ_DIR)/corpus/seed-fragment
	touch $@

fuzz: $(FUZZ_DIR)/mib-fuzz $(FUZZ_DIR)/corpus/seeded
	cd "$(FUZZ_DIR)" && MIB_OCR=0 ./mib-fuzz -max_total_time=$(FUZZ_TIME) -max_len=65536 -artifact_prefix=./ corpus $(ARGS)

# Optional native workflows in an isolated GNOME/Mutter compositor.
test-linux-e2e: mib
	$(PYTHON) tools/test-linux-e2e.py --binary "$(LINUX_BUILD_DIR)/mib" --output "$(LINUX_BUILD_DIR)/e2e" $(ARGS)

# Optional Vim parity check: types tests/vim-compare.toml into mib and Vim.
vim-compare: mib
	$(PYTHON) tools/vim-compare.py --binary "$(LINUX_BUILD_DIR)/mib" --output "$(LINUX_BUILD_DIR)/vim-compare" $(ARGS)

page-preview: $(TOOL_BIN)/page-preview

$(TOOL_BIN)/page-preview: tools/page-preview.c $(CORE) $(HEADERS) $(FONT_DATA) $(MEMORY) $(FILES) | $(TOOL_BIN)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) $(LDFLAGS) -o $@ tools/page-preview.c $(CORE) $(MEMORY) $(FILES) $(LDLIBS)

demo-frames: $(TOOL_BIN)/demo-frames

$(TOOL_BIN)/demo-frames: tools/demo-frames.c $(CORE) $(HEADERS) $(FONT_DATA) $(MEMORY) $(FILES) | $(TOOL_BIN)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) $(LDFLAGS) -o $@ tools/demo-frames.c $(CORE) $(MEMORY) $(FILES) $(LDLIBS)

# Record the README demos, docs/demo.svg and docs/demo-ocr.svg, from the real editor, with MP4
# copies (docs/demo.mp4, docs/demo-ocr.mp4) for places that don't play animated SVGs.
demo: $(TOOL_BIN)/demo-frames
	$(PYTHON) tools/demo.py "$(TOOL_BIN)/demo-frames" editing docs/demo.svg --frames "$(LINUX_BUILD_DIR)/demo-frames" --mp4 docs/demo.mp4 $(ARGS)
	$(PYTHON) tools/demo.py "$(TOOL_BIN)/demo-frames" ocr docs/demo-ocr.svg --frames "$(LINUX_BUILD_DIR)/demo-ocr-frames" --mp4 docs/demo-ocr.mp4 $(ARGS)

font-preview: $(TOOL_BIN)/font-preview

$(TOOL_BIN)/font-preview: tools/font-preview.c src/editor.c src/keys.c src/ex.c src/history.c src/substitute.c src/tiny-font.c $(HEADERS) $(FONT_DATA) $(MEMORY) $(FILES) | $(TOOL_BIN)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(INCLUDES) $(LDFLAGS) -o $@ tools/font-preview.c src/editor.c src/keys.c src/ex.c src/history.c src/substitute.c src/tiny-font.c $(MEMORY) $(FILES) $(LDLIBS)

subset-font: | $(GEN)
	$(PYTHON) tools/subset-font.py fonts/JetBrainsMonoNerdFontMono-Regular.ttf fonts/editor.tvf
	$(PYTHON) tools/embed-font.py fonts/editor.tvf $(FONT_DATA)

# compile_commands.json for clangd: Linux files use these flags, Windows files Zig's mingw headers.
compile-commands: $(FONT_DATA) $(PROTOCOL_HEADERS) $(GEN)/version.h
	$(PYTHON) tools/compile-commands.py -- $(CPPFLAGS) $(CFLAGS) $(INCLUDES) $(WAYLAND_CFLAGS)

format:
	$(CLANG_FORMAT) -i $(FORMAT_FILES)

format-check:
	$(CLANG_FORMAT) --dry-run -Werror $(FORMAT_FILES)

memory: mib
	$(PYTHON) tools/measure-memory.py --binary "$(LINUX_BUILD_DIR)/mib"

clean:
	rm -rf "$(LINUX_BUILD_DIR)"
