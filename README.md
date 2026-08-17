# Jot

<img src="packaging/dev.jot.Jot.svg" width="96" height="96" alt="Jot logo">

A small C editor for notes and screenshots with native Linux/Wayland and Windows
backends, rendering on the CPU. Redraws are capped at 30 FPS and stop when idle;
input continues to be processed immediately.
Its bundled monospace Nerd Font uses a small custom outline format, an 8×8
supersampling rasterizer, and a bounded glyph cache. There are no runtime
FreeType, HarfBuzz, Fontconfig, or system-font dependencies. Fractional scaling
rasterizes fresh glyphs at the actual physical pixel size.

## Build and run (Linux)

Requires a C compiler, Make, pkg-config, Python 3 (standard library), Wayland and
libxkbcommon development files, `wayland-scanner`, and `wayland-protocols` with
xdg-shell, viewporter, fractional-scale-v1, xdg-activation-v1, xdg-decoration,
cursor-shape, and tablet-v2. Tests also use
the Wayland server development package; Tesseract tests are optional. Normal builds use the checked-in
19,164-byte subset; there are no downloads or font-library build steps.

```sh
make
./.build/linux/jot notes.txt
# Open a scratch page:
./.build/linux/jot
./.build/linux/jot work
./.build/linux/jot --page ./scratch
# Or start an unnamed, manually saved buffer:
./.build/linux/jot --file -
```

Make outputs stay under `.build/`: the Linux editor is
`.build/linux/jot`, test executables are in `.build/linux/tests/`, preview
tools are in `.build/linux/tools/`, and generated headers/protocols are in
`.build/linux/generated/`. Windows builds remain in `.build/windows/`.
`make run` and `make run-windows` build and launch the appropriate executable.
`make clean` removes the Linux Make build, leaving Windows builds and Wine
profiles intact.

Install the Linux editor to `~/.local/bin/jot` without sudo:

```sh
make install
```

`make install` builds the editor first. Override `PREFIX` (default `$(HOME)/.local`)
or `BINDIR` (default `$(PREFIX)/bin`) to choose the destination. `DESTDIR` adds a
staging root for packaging, for example `make install DESTDIR=/tmp/jot-package`.
Installation also adds a **Jot** launcher entry at
`~/.local/share/applications/dev.jot.Jot.desktop`. Its executable path follows
`BINDIR`; override `DATADIR` or `APPLICATIONSDIR` to choose where the desktop entry
is installed. The logo is installed under `$(DATADIR)/icons/hicolor` (override
`ICONDIR` to change the icon root). The font remains embedded.
This replaces the previous Jot executable and launcher entry when installed to
the same prefix. Existing pages and preferences remain compatible.

The logo is the unchanged [upstream Jot SVG](https://github.com/osigurdson/jot/blob/4cdc1ccb336dab9237f97394b70bd8704c1758b3/data/dev.jot.Jot.svg),
covered by the bundled [MIT license](third_party/jot-LICENSE).

Wayland window decorations are negotiated with the compositor. Jot requests
server-side decorations, keeping tiling desktops such as Hyprland minimal.
When the compositor requires client decorations or does not support negotiation
(as on GNOME), Jot draws a compact title bar with drag, minimize, maximize, and
close controls. Double-click the title to maximize/restore; drag an edge to
resize. The edges have eight-pixel grab areas and larger corner targets; resize
cursors follow the desktop theme when cursor-shape is available. A small grip
marks the bottom-right corner. The title bar follows the light/dark palette and
hides in fullscreen.

Run inside a Wayland session. File-looking arguments (a name with an extension)
open explicit files; missing files start empty. `--file path` forces that mode.
Other arguments follow jot's page conventions: no argument uses `$JOT_DIR` or
`~/Documents/jot/default` (`~/jot/default` without Documents), a bare name chooses
a sibling page, and a path chooses a page folder. `--page folder` forces page
mode. Pages contain `jot.md`, private `assets/`, and agent instructions on first
creation. Save pages explicitly with `:w` or `Ctrl+S`. Pages reload external
changes on focus when no local edits are pending. One process owns a page; a
second launch requests activation of that window. Pages and explicit files both
protect unsaved changes on quit.

Markdown presentation supports headings, nested bullet/numbered/task lists,
quotes, horizontal rules, fenced and inline code, links, bare URLs, images,
bold/italic combinations, strikethrough, and simple pipe tables. Tables have
bold headers, left/center/right alignment, escaped pipes (`\|`), and wrapping
cell text. They support up to 16 columns with matching cell counts. Very wide
tables or rows taller than the viewport show as source so all text stays
accessible. Polish mode hides syntax, adds hanging list indents and heading separators, and stays read-only. Raw mode
always shows the exact source. Parsing uses fixed-size line storage; unusually
complex inline formatting falls back to literal text when that storage fills.

## Platforms and source organization

Linux/Wayland and Windows share the editor, application, renderer, and bundled
font code under `src/`. Native window, file, and memory code lives under
`src/platform/linux/` and `src/platform/windows/`. The Windows build uses Win32
and GDI and is tested under Wine. See [Windows build/run instructions and tested
limitations](docs/windows.md).

From Linux, use `make windows` to cross-build, `make run-windows` to build and
launch through Wine, or `make test-windows` to run the Windows tests. Pass editor
arguments with, for example, `make run-windows ARGS='--file notes.txt'`.

See [platform contracts, build commands, and platform implementation details](docs/platforms.md).
CMake selects the target backend without introducing Linux dependencies into a
Windows build:

```sh
cmake -S . -B .build/cmake -DCMAKE_BUILD_TYPE=MinSizeRel
cmake --build .build/cmake
ctest --test-dir .build/cmake --output-on-failure
```

## Keys

The shared editor implements normal, insert, visual and visual-line modes,
counts, operators/text objects, grouped undo/redo, smart-case incremental search,
Ex ranges/substitutions, clipboard paste/yank, wrapping, and conservative
Markdown styling. See the [complete key reference](docs/keys.md).

`Ctrl+S` saves, `Ctrl+Q` quits, and `Ctrl+plus/minus/0` changes the font size.
Choose a render mode with `:set raw`, `:set normal`, or `:set polish`:

- **Raw** shows plain source, including literal image markup, with no Markdown styling.
- **Normal** is the default: styled Markdown with visible syntax and inline images.
- **Polish** is a read-only reading view. It conceals supported Markdown markers,
  removes the gutter, centers the reading column, and adds breathing room.

Raw and normal remain editable and show line numbers. Polish keeps navigation,
search, selection, copying, saving, and link/image opening; switch to raw or
normal to edit. Selections and copies still refer to the underlying Markdown
source. These modes use the existing Markdown parser; no new Markdown syntax
is introduced. `:set md` and `:set nomd` remain aliases for normal and raw.

Search matches stay highlighted in every mode. `:set light` and `:set dark`
choose the color palette. Scratch pages remember both render mode and palette
when closed. Dark is the default. Saves always use the current page or file.
In raw and normal, paste screenshots or drop image files to import them. Images stay as
standalone `![](assets/name.png)` lines. `gx` or double-click opens an image;
`Ctrl+click` or `gx` opens a link. Right-click an image for Open/Copy Text.
Tesseract, when installed, runs one job at a time to create `.png.txt`/`.png.tsv`
sidecars used by search/highlighting. `TINYVI_OCR=0` disables automatic OCR.

Saves write a temporary file in the destination directory before renaming it,
so a failed write does not truncate the original. Existing file permissions are
preserved, and symlinks are followed on Linux. New Linux files use private
permissions (0600). Windows uses native file attributes/sharing checks and
replacement APIs; new files inherit the directory ACL. LF and CRLF files are
edited as LF internally and saved using the first input newline's style. New
buffers default to LF.

Editing preserves UTF-8 bytes, but display is limited to the bundled font
subset. Unsupported codepoints use a replacement glyph. Complex shaping,
combining sequences, wide characters, fallback fonts and input methods remain
unsupported. Regex/case operations implement a deliberately small subset; see
[feature scope and current verification](docs/jot-parity.md).

## Implementation and checks

`src/editor.c` holds the portable text buffer and file operations; `keys.c`,
`ex.c`, `substitute.c` and `history.c` implement the editing language. `session.c`
handles page persistence/ownership, while `media.c` handles asset references and
external OCR. PNG/JPEG codecs are vendored from stb with bounded allocations.
`src/app.c` handles shared input/lifetime/close policy and `src/render.c` paints
through the pitch-aware `app_paint` interface. `src/tiny-font.c` implements the
outline font and glyph cache. Native windows, clipboard, file and memory code
remain isolated under `src/platform/`.

Only two submitted pixel buffers can be pending at once. Rendering is coalesced
between event batches and happens only when needed. The font cache is a fixed
48 KiB alpha arena with 64 entries at the current target size. Temporary raster
workspace is unmapped after each frame. No allocation grows with the number of
fonts, glyphs, or sizes used.

```sh
make test
# Optional render preview, written as PPM:
./.build/linux/tests/client-test /tmp/jot.ppm
make clean
```

Tests cover modal editing, grouped undo, regex captures/ranges, UTF-8 boundaries,
safe file saves, page ownership/manual saves/reload, image round trips, OCR sidecar
search, input translation, DPI rendering and small-window wrapping. Private native clipboard integration tests avoid replacing the desktop clipboard;
see the parity report for their coverage and limits.

## Current footprint

The jot feature port is **159,320 bytes stripped on Linux** and **246,272 bytes
on Windows**. An empty page measured **777–792 KiB PSS** (660–664 KiB private),
while total RSS including shared libraries was 2,936–3,000 KiB. A large inline
image returned to 805–824 KiB PSS after painting, with a transient decoding peak
around 41 MiB. See [the feature scope, bounds, tests and before/after report](docs/jot-parity.md).

## Font assets and historical measurements

- [Font format, rasterizer bounds, supported Unicode, and limitations](docs/font-format.md).
- [Before/after size and RSS report](docs/font-pipeline-report.md).
- [FreeType versus custom visual comparison](docs/font-comparison.png).

The original custom-font executable measured 149,896 bytes (60,384 stripped).
After platform separation, the Make debug build measured 158,904 bytes and the
CMake MinSizeRel build 70,040 bytes. Idle memory remains consistent with the
custom-font version; see [platform verification](docs/platforms.md). Five
interleaved font-pipeline runs measured median idle RSS of 2,828 KiB, versus
2,824 KiB before; this was essentially unchanged. The previous version already unloaded FreeType
at idle. The dependency and font-file removal matters for deployment and active
rendering, rather than producing a large idle-RSS saving.

```sh
make memory           # Idle PSS/RSS for a real empty file in a Wayland session
make subset-font      # Optional: rebuild the subset; requires Python fonttools
make font-preview
./.build/linux/tools/font-preview /tmp/font-sizes.ppm
```

The source font and subset are bundled under the SIL Open Font License;
see `fonts/OFL.txt`, `fonts/NERD-FONTS-LICENSE.txt`, and `fonts/editor.json` for
provenance. The full TTF is used only to regenerate the subset. Arbitrary fonts
and runtime font selection are not supported. `make clean` removes generated
headers and binaries without deleting the bundled font assets.

Idle client RSS/PSS exclude the compositor's retained framebuffer. At 150%,
the 1200×840 XRGB8888 frame occupies about 3.85 MiB; client mappings are released
after painting. Startup and redraw peak memory are larger than idle memory.

Protocol and library references:
[Wayland](https://wayland.freedesktop.org/docs/html/apa.html),
[FontTools (subset generation only)](https://fonttools.readthedocs.io/en/latest/pens/index.html),
[libxkbcommon](https://xkbcommon.org/doc/current/md_doc_2quick-guide.html).
