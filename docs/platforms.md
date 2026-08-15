# Platform boundaries and Windows port plan

The current runnable editor is the Linux/Wayland version. Windows has a native
page-allocation implementation and explicit window/file stubs. It is not yet a
usable Windows editor. The stubs return errors rather than silently succeeding,
opening empty buffers for unreadable files, or losing unsaved edits.

## Source layout

```text
src/
  main.c                  argument handling and application lifetime
  app.h, app.c            shared editor state, normalized input, close policy
  editor.h, editor.c      text buffer, movement, modes, commands, UTF-8 traversal
  render.c                shared CPU UI renderer and physical-pixel layout
  tiny-font.h, tiny-font.c bundled-font reader, rasterizer, bounded cache
  platform.h              small platform contract; no OS headers
  platform/
    linux/
      wayland.c           window, xdg-shell, display scale, keyboard, event loop
      files.c             regular-file reads and atomic saves
      memory.c            mmap/munmap and optional glibc heap trimming
    windows/
      window_stub.c       explicit failure; future Win32 window/event backend
      files_stub.c        explicit failures; future Unicode/atomic file backend
      memory.c            VirtualAlloc/VirtualFree, ready for native build
```

Generated font and Wayland protocol code lives in `.build/generated/` for Make,
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
./tiny-vi notes.txt
```

CMake, on Linux:

```sh
cmake -S . -B .build/cmake -DCMAKE_BUILD_TYPE=MinSizeRel
cmake --build .build/cmake
ctest --test-dir .build/cmake --output-on-failure
./.build/cmake/tiny-vi notes.txt
```

Windows, once using a native C11 compiler/Windows SDK and Python 3:

```powershell
cmake -S . -B build-windows
cmake --build build-windows --config MinSizeRel
ctest --test-dir build-windows -C MinSizeRel --output-on-failure
```

CMake selects Windows sources using the target OS (`WIN32`). It does not query
pkg-config, Wayland, xkbcommon, or wayland-scanner on Windows. The resulting
executable currently reports that its window backend is not implemented and
exits unsuccessfully. Supplying a filename reports the unimplemented file
backend instead. No working native Windows build was verified on the Linux
development host, which has no Windows compiler/SDK.

The Windows file/window failure contracts are also compiled and tested on Linux
with `platform-stub-test`, using Linux page allocation. This verifies shared-core
compatibility and safe stub behavior; it does not verify Win32 ABI integration.

## Remaining Windows work

1. Replace `window_stub.c` with a Win32 class/window and event loop. Normalize
   keys/text into `app_input`, honor close protection, handle key repeat/focus,
   and convert UTF-16 surrogate pairs into committed UTF-8.
2. Handle per-monitor DPI changes, updating logical geometry and `scale120`.
   Paint into a top-down 32-bit DIB at the physical pixel size and present it
   without scaling already-rasterized text.
3. Replace `files_stub.c` with UTF-8-to-UTF-16 path handling, binary read streams,
   regular-file checks, and sibling-temporary-file/flush/atomic replacement.
   Define Windows permission, symlink, sharing, and rename-error semantics.
4. Handle Unicode command-line paths at the Windows entry point, and add native
   Windows file/keyboard/DPI tests. Decide separately on GUI packaging and an
   executable icon; the current stub is a console target for visible errors.

The Linux file behavior, editor commands, glyph cache, and font format do not
need another rewrite for these steps.

## Verification after the split

Make tests and CMake/CTest pass on Linux. Portable code and the file/window stubs
pass a strict C11 syntax check. Tests cover pitched versus tightly packed frame
buffers, input/mode transitions, close protection, file failures, and stub save
failure preserving the document/modified flag. Native Linux document and cursor
pixels match the pre-split renderer. A smoke test still maps a 1200×840 buffer
at 150% scale.

An initial Make build measured 158,904 bytes (including debug information);
the CMake MinSizeRel build measured 70,040 bytes without debug information.
Empty-file idle samples were 807–863 KiB PSS and 2,756–2,824 KiB RSS on the same
Wayland host. This remains consistent with the earlier custom-font version;
GNOME System Monitor may report a different memory category.
