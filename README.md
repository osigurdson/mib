# Tiny vi

A small C text editor that talks directly to Wayland. FreeType renders the text
on the CPU into shared-memory buffers; fractional scaling keeps it sharp on
HiDPI displays. There is no GUI toolkit or GPU rendering in the client.

## Build and run

Requires a C compiler, Make, pkg-config, and development files for Wayland,
FreeType, Fontconfig, and libxkbcommon. Also install `wayland-scanner` and
`wayland-protocols` with xdg-shell, viewporter, and fractional-scale-v1.

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
language. Basic UTF-8 codepoints are supported; complex shaping, combining
sequences, wide characters, font fallback, and input methods are not implemented.

## Implementation and checks

`editor.c` holds the text buffer, movement, modes, and file operations. `hello.c`
contains the Wayland client, keyboard mapping, and FreeType renderer.
Fontconfig's discovery data is released after loading the monospace font.
Only two submitted pixel buffers can be pending at once; rendering is coalesced
between event batches and happens only when needed.

```sh
make test
# Optional render preview, written as PPM:
./client-test /tmp/tiny-vi.ppm
make clean
```

Tests cover editing, UTF-8 boundaries, movement, save/quit behavior, write
failures, keyboard translation, and rendering/scrolling at small window sizes.

Protocol and library references:
[Wayland](https://wayland.freedesktop.org/docs/html/apa.html),
[FreeType](https://freetype.org/freetype2/docs/tutorial/step1.html),
[libxkbcommon](https://xkbcommon.org/doc/current/md_doc_2quick-guide.html).
