# Tiny vi

A small C text editor that talks directly to Wayland and renders on the CPU.
Its bundled monospace Nerd Font uses a small custom outline format, an 8×8
supersampling rasterizer, and a bounded glyph cache. There are no runtime
FreeType, HarfBuzz, Fontconfig, or system-font dependencies. Fractional scaling
rasterizes fresh glyphs at the actual physical pixel size.

## Build and run

Requires a C compiler, Make, pkg-config, Python 3 (standard library), Wayland and
libxkbcommon development files, `wayland-scanner`, and `wayland-protocols` with
xdg-shell, viewporter, and fractional-scale-v1. Normal builds use the checked-in
19,164-byte subset; there are no downloads or font-library build steps.

```sh
make
./tiny-vi notes.txt
# Or start an unnamed buffer:
./tiny-vi
```

Run inside a Wayland session. A missing file opens as a new, empty buffer.

## Keys

The editor starts in normal mode.

| Key | Action |
| --- | --- |
| `h` / `j` / `k` / `l` | Left / down / up / right in normal mode |
| `i` | Enter insert mode |
| `Esc` | Return to normal mode, or cancel a command |
| Text, Enter, Tab | Insert in insert mode |
| Backspace / Delete | Delete before / under the cursor in insert mode |
| Arrow keys | Move in either mode |
| `:w` + Enter | Save the current file |
| `:w path` + Enter | Save under a filename (required for an unnamed buffer) |
| `:q` + Enter | Quit if there are no unsaved changes |
| `:wq` + Enter | Save, then quit if the save succeeds |
| `:wq path` + Enter | Save under a filename, then quit |
| `:q!` + Enter | Discard changes and quit |

The status bar shows the mode, filename, modification flag, and cursor position.
The view scrolls to follow the cursor, including across long lines. Tabs occupy
four-column stops. Holding a key follows the compositor's repeat settings.
Closing the window also protects unsaved changes.

Saves write a temporary file in the destination directory before renaming it,
so a failed write does not truncate the original. Existing file permissions are
preserved, and symlinks are followed. New files use private permissions (0600).

This is a minimal editor: no undo, search, selections, clipboard, or Vim command
language. Editing preserves UTF-8 bytes, but display is limited to the bundled
subset; unsupported codepoints use a replacement glyph. Complex shaping,
combining sequences, wide characters, font fallback, and input methods are not
implemented.

## Implementation and checks

`editor.c` holds the text buffer, movement, modes, and file operations. `hello.c`
contains the Wayland client and renderer integration; `tiny-font.c` implements
the small font reader, contour flattener, scan converter, and glyph cache. The
existing `paint`/glyph interface is retained. The UI uses bundled Nerd Font
file, pencil, and modified-state icons.

Only two submitted pixel buffers can be pending at once. Rendering is coalesced
between event batches and happens only when needed. The font cache is a fixed
48 KiB alpha arena with 64 entries at the current target size. Temporary raster
workspace is unmapped after each frame. No allocation grows with the number of
fonts, glyphs, or sizes used.

```sh
make test
# Optional render preview, written as PPM:
./client-test /tmp/tiny-vi.ppm
make clean
```

Tests cover editing, UTF-8 boundaries, movement, save/quit behavior, write
failures, keyboard translation, and rendering/scrolling at small window sizes.

## Font assets and measurements

- [Font format, rasterizer bounds, supported Unicode, and limitations](docs/font-format.md).
- [Before/after size and RSS report](docs/font-pipeline-report.md).
- [FreeType versus custom visual comparison](docs/font-comparison.png).

The default executable is 149,896 bytes (60,384 stripped). Five interleaved
empty-file runs measured median idle RSS of 2,828 KiB, versus 2,824 KiB before;
this is essentially unchanged. The previous version already unloaded FreeType
at idle. The dependency and font-file removal matters for deployment and active
rendering, rather than producing a large idle-RSS saving.

```sh
make memory           # Idle PSS/RSS for a real empty file in a Wayland session
make subset-font      # Optional: rebuild the subset; requires Python fonttools
make font-preview
./font-preview /tmp/font-sizes.ppm
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
