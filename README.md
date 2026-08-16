# Tiny vi

A small C text editor with native Linux/Wayland and Windows backends, rendering
on the CPU.
Its bundled monospace Nerd Font uses a small custom outline format, an 8×8
supersampling rasterizer, and a bounded glyph cache. There are no runtime
FreeType, HarfBuzz, Fontconfig, or system-font dependencies. Fractional scaling
rasterizes fresh glyphs at the actual physical pixel size.

## Build and run (Linux)

Requires a C compiler, Make, pkg-config, Python 3 (standard library), Wayland and
libxkbcommon development files, `wayland-scanner`, and `wayland-protocols` with
xdg-shell, viewporter, fractional-scale-v1, and xdg-activation-v1. Tests also use
the Wayland server development package; Tesseract tests are optional. Normal builds use the checked-in
19,164-byte subset; there are no downloads or font-library build steps.

```sh
make
./.build/linux/tiny-vi notes.txt
# Open an autosaved scratch page:
./.build/linux/tiny-vi
./.build/linux/tiny-vi work
./.build/linux/tiny-vi --page ./scratch
# Or start an unnamed, manually saved buffer:
./.build/linux/tiny-vi --file -
```

Make outputs stay under `.build/`: the Linux editor is
`.build/linux/tiny-vi`, test executables are in `.build/linux/tests/`, preview
tools are in `.build/linux/tools/`, and generated headers/protocols are in
`.build/linux/generated/`. Windows builds remain in `.build/windows/`.
`make run` and `make run-windows` build and launch the appropriate executable.
`make clean` removes the Linux Make build, leaving Windows builds and Wine
profiles intact.

Run inside a Wayland session. File-looking arguments (a name with an extension)
open explicit files; missing files start empty. `--file path` forces that mode.
Other arguments follow jot's page conventions: no argument uses `$JOT_DIR` or
`~/Documents/jot/default` (`~/jot/default` without Documents), a bare name chooses
a sibling page, and a path chooses a page folder. `--page folder` forces page
mode. Pages contain `jot.md`, private `assets/`, and agent instructions on first
creation. They autosave after 400 ms without typing, save on focus loss, and
reload external changes on focus when no local edits are pending. One process
owns a page; a second launch requests activation of that window. Explicit files
retain manual saves and unsaved-change protection.

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
`:set nu/nonu` toggles line numbers, `:set md/nomd` toggles Markdown styling.
Screenshots can be pasted or imported by dropping image files. Images stay as
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
./.build/linux/tests/client-test /tmp/tiny-vi.ppm
make clean
```

Tests cover modal editing, grouped undo, regex captures/ranges, UTF-8 boundaries,
safe file saves, page ownership/autosave/reload, image round trips, OCR sidecar
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
