# Platform boundaries and backends

Linux/Wayland and native Windows backends are implemented. Both share the
editor, CPU renderer, and bundled font. Windows builds and automated tests were
run under Wine; see [Windows build and test details](windows.md).

## Source layout

```text
src/
  main.c                  shared argument handling and application lifetime
  app.h, app.c            shared app state, normalized input, close policy
  editor.h, editor.c      text buffer, cursor movement, UTF-8 traversal
  keys.c                  normal, visual and insert mode keys, motions, operators
  ex.c, substitute.c      : commands and the :s regex subset
  history.c               every text change, grouped undo/redo, mark shifting
  pages.c, session.c      the nine pages in the notes folder and where it lives
  media.h, media.c        pasted images, clipboard fragments, OCR and image search
  image.h, image.c        PNG/JPEG decoding and PNG encoding
  markdown.h, markdown.c  Markdown blocks, spans and tables for drawing
  render.c                shared CPU renderer and physical-pixel layout
  navigator.c, help.c     the page list and the :help card, drawn on top of render.c
  colors.h, theme-palettes.h  color schemes (see colors.md)
  tiny-font.h, tiny-font.c    bundled-font reader, rasterizer, bounded cache
  platform.h              small platform contract; no OS headers
  platform/
    linux/
      entry.c             Linux main entry
      wayland.c           window, xdg-shell/decorations, display scale, input, event loop
      clipboard.inc       Wayland clipboard and drag and drop
      files.c             regular-file reads and atomic saves
      memory.c            mmap/munmap and optional glibc heap trimming
    windows/
      entry.c             Unicode wmain entry and argument conversion
      window.c            Win32 window/messages, DPI, CPU bitmap presentation
      clipboard.inc       native clipboard and shell actions
      files.c             Unicode reads and temporary-file replacement
      utf.c, win32.h      UTF-8/UTF-16 paths and Windows errors
      memory.c            VirtualAlloc/VirtualFree
```

Generated font and Wayland protocol code lives in `.build/linux/generated/` for Make,
or the CMake build directory's `generated/` subdirectory. Generated artifacts
never belong in `src/` or the repository root.

## Shared contracts

`platform.h` selects implementations at link/build time. There is no runtime
backend registry, dispatch table, platform branch in the editor, or added GUI
framework.

- `platform_run(app)` owns native window/input/presentation resources and the
  event loop. It returns zero on a clean exit and nonzero on failure. The shared
  main owns the app and destroys it after the backend returns.
- `app_input` accepts `enum editor_key` plus committed UTF-8 text. Native keycodes,
  keyboard layouts, repeats, and input methods belong to each window backend.
- `app_request_close` protects unsaved changes for either OS. Window backends
  should not bypass it when the user closes a window.
- Logical window width/height and a `scale120` ratio live in `app`. A backend
  updates those fields and marks `dirty` on size/DPI changes. It supplies actual
  physical dimensions to `app_paint`.
- `app_paint` writes top-down `0xFFRRGGBB` pixels. Stride is measured in `uint32_t`
  pixels and may exceed width. Rendering does not allocate or present a native
  window surface. On little-endian Linux/Windows this is BGRA byte order with
  opaque alpha, suitable for XRGB8888 or a top-down 32-bit DIB.
- File paths are UTF-8 at the shared boundary. `platform_file_open_read`
  distinguishes a missing file from an error and returns a binary `FILE*` that
  the caller closes. The model handles buffering and rejects NUL-containing
  binary contents. `platform_file_write_atomic` completes replacement before
  reporting success; command handling alone changes the modified flag.
- Font allocations request dedicated, zero-filled pages from
  `platform_allocate_zeroed` and release them with the original size. Font cache
  and temporary-workspace limits are unchanged. Heap trimming is optional and
  affects neither ownership nor correctness.

OS headers and APIs belong under `src/platform/`. Shared code uses C11 and the
C standard library only; it no longer includes `mmap`, POSIX file functions,
Wayland, xkbcommon, or Windows headers. The renderer and font assets are the
same on both targets.

## Building

Linux convenience build:

```sh
make
make test
./.build/linux/mib
```

CMake, on Linux:

```sh
cmake -S . -B .build/cmake -DCMAKE_BUILD_TYPE=MinSizeRel
cmake --build .build/cmake
ctest --test-dir .build/cmake --output-on-failure
./.build/cmake/mib
```

Windows builds, cross-compilation, Wine commands, coverage, and limitations are
in [windows.md](windows.md). CMake selects source files based on the target OS.
It does not query Wayland, xkbcommon, or wayland-scanner for a Windows build.

The shared `app_main` function owns argument validation and app lifetime;
`platform/linux/entry.c` provides `main`, and `platform/windows/entry.c` provides
`wmain` with UTF-16-to-UTF-8 conversion. OS-specific command-line handling stays
outside the shared model and renderer.

## Verification

`make test` runs the shared and Linux tests; `make test-windows` cross-builds and runs the
Windows tests under Wine. [windows.md](windows.md) lists what those cover and what still
needs a real Windows machine.
