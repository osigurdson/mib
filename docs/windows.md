# Windows version and Wine verification

The Windows backend is implemented using Win32, User32, and GDI. It shares the
editor, CPU renderer, bundled Nerd Font subset, and glyph cache with Linux.
Windows 10 version 1703 or newer is the target (per-monitor DPI V2 APIs). No SDL, FreeType,
Fontconfig, HarfBuzz, or external font file is needed at runtime.

## Build on Windows

Use a C11 compiler/Windows SDK, CMake 3.20+, and Python 3. For example, from a
Visual Studio developer terminal:

```powershell
cmake -S . -B build-windows
cmake --build build-windows --config MinSizeRel
ctest --test-dir build-windows -C MinSizeRel --output-on-failure
.\build-windows\MinSizeRel\jot.exe
```

With a single-configuration generator, the executable is directly in the build
directory. `jot.exe` is a GUI program, so starting it opens no console window. It keeps
the Unicode `wmain` entry point (`/ENTRY:wmainCRTStartup` with MSVC, `-municode` with
MinGW and Zig), and shows usage and startup errors in a message box. Desktop packaging
(Start menu entry, icon) is not provided by this build.

## Cross-build on Linux and run under Wine

Zig provides the mingw-w64 headers and Windows libraries; no separate MinGW
installation is required. Verified with Zig 0.17.0 and Wine 11.21 on this host.
CMake detects Wine as the cross-compiling emulator for tests.

The Makefile wraps configuration, building and Wine launch:

```sh
make windows                         # build .build/windows/jot.exe
make run-windows                     # build and open the Windows editor
make test-windows                    # build and run the Wine test suite
```

These targets default to the isolated `.build/wine-prefix`. Override
`WINEPREFIX`, `WINE`, or `WINDOWS_BUILD_DIR` when needed. Each target configures the build automatically, so no separate setup
command is required.

The equivalent CMake commands are:

```sh
cmake -S . -B .build/windows -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=tools/cmake/zig-windows.cmake \
  -DCMAKE_BUILD_TYPE=MinSizeRel -DBUILD_TESTING=ON
cmake --build .build/windows --parallel 4
./tools/test-wine.sh
WINEPREFIX="$PWD/.build/wine-prefix" wine .build/windows/jot.exe
```

The test script creates/uses `.build/wine-prefix`, independently of an existing
`~/.wine`. Wine needs a working graphical display (the existing desktop worked
for these tests). Set `ZIG` when the compiler is not named `zig` on PATH. A native
MinGW toolchain can also be used with an appropriate CMake toolchain file.

The measured x86-64 MinSizeRel executable is **246,272 bytes** (baseline: 98,304 bytes). It imports Windows
system/User32/GDI/Universal CRT DLLs only. No separately shipped font DLL or TTF
is required. Wine's own process memory is not a native Windows memory benchmark.

## Behavior and ownership

- `entry.c` converts UTF-16 command-line arguments into the shared UTF-8 API.
- `window.c` handles a Unicode Win32 class, message loop, focus, key repeat,
  arrows/Delete, translated control keys, and `WM_CHAR`/`WM_UNICHAR` text.
  UTF-16 surrogate pairs become UTF-8; out-of-subset characters are stored
  correctly even when displayed as replacement glyphs. Alt+F4/window close
  honors the same save/close policy as `:q` (unsaved changes require saving or explicit discard).
- Per-monitor DPI awareness is requested before creating a window. DPI changes
  apply Windows' suggested rectangle, update logical geometry and `scale120`,
  and rerasterize glyphs at the physical size. Paint tests exercise 96, 120,
  144, and 192 DPI (100%, 125%, 150%, 200%). These are message-path tests,
  not a claim that four physical monitors were tested.
- Each paint allocates a temporary top-down 32-bit buffer, calls `app_paint`,
  presents it with `StretchDIBits` at identical source/destination dimensions,
  flushes GDI, and releases the buffer. There is no persistent full-window pixel
  allocation and no bitmap scaling of the text. Windows handles presentation.
- `files.c` converts UTF-8 paths to extended-length UTF-16 paths, opens binary
  streams, and rejects non-disk files/directories. Existing links/reparse points
  are resolved to their final file target when saving.
- Saves use a uniquely created sibling temporary file, complete bounded writes,
  `FlushFileBuffers`, and close before replacement. Existing files use
  `ReplaceFileW`; new files use `MoveFileExW` without overwriting a destination
  that appeared meanwhile. Read-only attributes and sharing/lock failures are
  reported with the document still modified. Existing metadata preservation is
  delegated to `ReplaceFileW`; new files inherit the destination directory ACL.
  Tests ensure temporary-name collisions never delete someone else's file.
- UTF-8 LF and CRLF text is supported on both platforms. Internally, CRLF becomes
  LF for cursor/layout consistency. Saving preserves the first input newline's
  style; mixed files are normalized to that style. New buffers default to LF.
  NUL-containing files (including UTF-16-encoded text) are rejected. Other font
  and editor limitations remain those in the README.

## Test results

Nine tests pass under Wine. The original platform checks cover:

1. All subset glyphs, alpha rasterization, font format checks, and cache bounds
   using the Windows `VirtualAlloc`/`VirtualFree` backend.
2. Shared input, close protection, and padded-row rendering.
3. Unicode/long paths, regular-file checks, save replacement, large writes,
   read-only/locked save failures, temporary-name collisions, and CRLF handling.
4. Win32 window messages, key repeat, surrogate pairs, close protection, failed
   `:wq`, four DPI paint paths, and GDI rendering into a captured bitmap.
5. The actual `jot.exe`, launched with `CreateProcessW`, driven through its
   real window: new/load/edit/`:w`/`:q`/`:wq`, filename spaces/BMP/non-BMP Unicode,
   UTF-8 text, close rejection for unsaved changes, and CRLF round-trip.

The GDI-rendered capture in `.build/windows/windows-editor.ppm` was inspected:
text, cursor, status line, and Markdown styling render correctly. Test fixtures
and the capture are in the build/Wine directories, not the source tree.
The expanded suite also checks modal editing/substitutions/undo, scratch pages
and saved preferences, image codecs/inline rendering/OCR sidecars, and private
clipboard Unicode/PNG/DIB/file/fragment serialization. The private clipboard
store prevents the test from replacing the desktop clipboard. Native Linux
checks additionally exercise a private Wayland data-device server and a real
Tesseract child when installed. See [the feature/footprint report](jot-parity.md).

## Observed Wine limitation and remaining native testing

On Wine 11.21, `ReplaceFileW` fails with error 3 for an **existing destination
whose full path exceeds 260 characters**. The test verifies that the original
file remains intact and the editor keeps unsaved changes. Creating and reading
long paths worked. There is no Wine-specific fallback that weakens replacement
or metadata semantics. Use a shorter path or `:w` to a different filename when
running under this Wine version.

Wine's implementation uses a fixed `MAX_PATH` buffer in `ReplaceFileW`; see
[Wine's file implementation](https://github.com/wine-mirror/wine/blob/master/dlls/kernelbase/file.c).
The native Windows API supports extended-length Unicode paths; see
[ReplaceFileW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-replacefilew).
Native Windows/NTFS permissions, hardware DPI transitions, and IME/keyboard
layout behavior were not tested on physical Windows here. File sharing and
replacement durability under Wine are not a substitute for those native checks.
