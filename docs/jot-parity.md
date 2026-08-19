# Jot feature port and footprint report

Reference: [osigurdson/jot](https://github.com/osigurdson/jot) at
`4cdc1ccb336dab9237f97394b70bd8704c1758b3` (README, key reference, implementation
and tests). The port uses native C/Wayland and Win32/GDI, the existing bundled
outline font, and CPU rendering. No GTK, SDL, Pango, FreeType, HarfBuzz or
Fontconfig dependencies were added. Native presentation differs from GTK.

## Delivered behavior

- Normal, insert, visual and visual-line modes; counts, documented motions,
  operators, text objects, replace/case/join/indent operations and grouped undo.
- Incremental smart-case text/OCR search, word search, next/back/wrapping,
  highlighted text and OCR word/phrase rectangles. Insert mode hides highlights
  while retaining the query; a new search or `n`/`N` shows them again. A pending
  failed search retries when OCR completes; another editing/navigation key cancels the retry.
- Ex ranges, capture substitutions, command history, save/reload/quit,
  line numbers, Markdown toggles, folder opening and zoom.
- Default/named/path scratch pages, private Linux folders, agent instructions
  created only for fresh pages, explicit saves, focus reload,
  persisted dimensions/maximized/font/options and one owner per page.
- Native text/image clipboard, cut/yank/put and visual replacement, PNG/JPEG
  file import, URI/file drops, inline images, image/link opening and image
  Open/Copy Text context actions. Image copy also offers plain Markdown text.
- Mixed text/image copies between pages import new asset files using an
  internal clipboard fragment format. Copying leaves original assets untouched.
- Conservative Markdown colors/weight with visible syntax, fenced code,
  headings/links/lists/quotes/checked items, wrapping, mouse selection and scroll.
- Optional, sequential Tesseract child jobs producing private `.png.txt` and
  `.png.tsv` files. Existing sidecars work without Tesseract. `TINYVI_OCR=0`
  disables automatic jobs. No OCR library remains loaded in the editor.

See [the key reference](keys.md). The reference excludes dot repeat, macros,
marks, named registers, block visual mode, plugins/config and exact Vim regex
compatibility; those remain outside this port too.

Explicit files retain manual saves and unsaved-change protection. A filename
extension selects file mode for compatibility; `--file path` forces file mode,
`--file -` starts an unnamed buffer and `--page folder` forces a scratch page.
No argument uses `$JOT_DIR` or `~/Documents/jot/default` (`~/jot/default` without
Documents). Other arguments follow jot's named/path page conventions.

## Image deletion and saving

Image deletion changes the buffer first. After a successful save, Jot removes
tracked images whose asset paths no longer appear anywhere in the note, and
removes their `.txt`/`.tsv` OCR sidecars. An active OCR job for a removed image is
stopped before cleanup. Failed saves leave assets and undo history alone.
Cleanup failures are reported after saving and retried on subsequent saves in
the same session.

A save that removes images resets the entire undo/redo history, and clears the
local register if it references a removed image. Other saves preserve history.
References in other notes or external clipboards do not keep a deleted image
alive. This assumes each page owns its assets; explicit files sharing an assets
folder need care. Before saving, undo and discarding changes still work normally.

Tracking is bounded to 256 distinct asset names observed or imported during a
session. Extra names or allocation failures conservatively leave files behind.
Current-text reference checks cover the whole note, including raw/inline text
and references beyond the inline image limit. Existing orphan files are not
swept. A crash after saving but before cleanup may leave an unused asset behind.

## Deliberate limits

- UTF-8 bytes are preserved and navigation respects codepoint boundaries. Case
  operations/smartcase are ASCII; no grapheme shaping, wide-character cells,
  fallback fonts or IME. Font coverage and unsupported font features are in
  [font-format.md](font-format.md); the font subset remains unchanged.
- The bounded substitution VM supports literals, dot, anchors, classes/ranges,
  `*`, `\+`, `\?`, `\(groups\)`, `\|`, `\<`/`\>`, `&` and `\1`–`\9` replacements.
  No lookaround or complete Vim/PCRE syntax. Regex work/backtracking is bounded.
- Undo is lazy: at most 2 MiB of delta payload plus 1,024 record descriptors.
  An insert session is one transaction. An oversized transaction clears undo
  rather than retaining an incomplete undo step.
- PNG/JPEG decoding and PNG encoding only; no animated/multipage images.
  At most 256 inline image references per page, 8 megapixels per image,
  32 MiB encoded image/clipboard transfer, 16 MiB per text put, and 64 MiB of
  concurrent codec allocations. Extra references remain editable Markdown.
- Images fit the viewport, retaining original source pixels when they fit;
  very tall images are reduced to fit its height. Decoded pixels are released
  after painting. No decoded-image cache is retained at idle.
- Each OCR sidecar read is capped at 2 MiB. Phrase highlights reconstruct up to
  256 words/8,192 text bytes per OCR line. Search visits a matching image as one
  result and highlights its matching words/phrases.
- Mixed clipboard fragments refer to the original page's persistent asset
  files until pasted. Those files must still exist. Single-image PNG clipboard
  copies are self-contained. Unsupported rich clipboard formats use plain text.
- Preferences are stored per page. Linux uses the literal Documents directory
  when present; use `JOT_DIR` for a different root. Page activation relays a
  launcher's `XDG_ACTIVATION_TOKEN`; the Wayland compositor controls whether it
  raises the window. Windows uses a named mutex and window property.

The stb source is pinned under `third_party/stb/` with its license. Image codecs
use a capped allocator, run only when needed and add no runtime library.

## Measured size

Before is main commit `3edb4c5`; after is this port. Linux stripped baseline is
from the saved pre-change executable. The Windows baseline was rebuilt from that
Git tree with the same Zig toolchain. Sizes are bytes, not compressed downloads.

| Executable | Before | After |
| --- | ---: | ---: |
| Linux Make `-Os -g` | 161,056 | 677,616 |
| Linux stripped | 60,416 | 159,320 |
| Windows x64 MinSizeRel | 98,304 | 246,272 |

The Linux CMake MinSizeRel executable before stripping is 180,992 bytes. All
executables, including the Linux debug build, remain below 1 MiB. Linux imports
only Wayland client, libxkbcommon, libm and libc. Windows uses system/CRT DLLs;
Shell32 is loaded only for an external open action. No external font is needed.

## Measured memory

Three real Wayland launches per fixture on this host, after the first paint:

| Fixture | Idle PSS (KiB) | Idle RSS (KiB) | Private (KiB) | RSS high-water (KiB) |
| --- | ---: | ---: | ---: | ---: |
| Before: empty explicit file | 684–687 | 2,828 | 552–556 | 6,176–6,276 |
| After: empty explicit file | 771–772 | 2,872 | 660 | 6,164–6,176 |
| After: empty scratch page | 777–792 | 2,936–3,000 | 660–664 | 6,196–6,308 |
| After: 2880×1800 inline image | 805–824 | 2,968–3,032 | 692 | 41,656–41,940 |

Empty-page **PSS and private memory are below 1 MiB**. RSS includes shared
libraries and exceeds 1 MiB; it must not be confused with private/PSS figures.
PSS varies as other processes share libraries. Image decoding has a real
transient peak, despite the small idle footprint. These are editor-process
figures: compositor buffers, optional OCR children and external viewers are
separate. Wine memory is not a native Windows memory benchmark.

Reproduce with:

```sh
make all test
python3 tools/measure-memory.py
python3 tools/measure-memory.py --kind page
python3 tools/measure-memory.py --kind image --max-pss 4096
# To compare a saved executable:
python3 tools/measure-memory.py --binary /path/to/baseline
```

## Native Linux editing audit (2026-11-02)

`make test-linux-e2e` now launches the real Linux executable in a private headless
Mutter compositor, private D-Bus session, and isolated runtime/config/data
folders. Test-only Python/Gio code creates a virtual keyboard and owns a PNG or
text clipboard in that compositor. It does not inject keys into the user's
desktop, replace the user's clipboard, or add automation hooks to Jot.

Ten real-app workflows check End-key insertion, Unicode/CRLF text clipboard,
search with `n`/`N`, undo/redo, image paste/typing/navigation, save/reopen,
image-boundary deletion, repeated image paste, and normal-mode `x`. A seeded
500-action insert-mode run compares saved text with an independent Python
Unicode-string model at 25-action checkpoints, then checks whole-session
undo/redo. A large note contains 10,000 populated text lines, 50 standalone
distinct 512×256 PNGs, and 711,400 UTF-8 source bytes. It visits all 50 images by search,
performs 250 insert/undo/redo transactions, checks that edits do not autosave,
then explicitly saves and reloads and verifies text and asset files.

These workflows exposed and now cover incorrect insert-mode End placement,
image references split by Backspace/Delete or another image paste, normal-mode
`x` deleting only a marker, and last-line deletion consuming a preceding newline.
Normal rendering now deletes selected/adjacent images as blocks for these keys;
raw mode still permits literal source editing. The real-app sanitizer run also
found a pending Wayland buffer proxy left allocated on quit. Both in-flight
buffers are now tracked and destroyed on shutdown; a private protocol test
covers release of one frame followed by shutdown with the other still pending.

All nine Linux Make tests, all nine Windows/Wine tests, and all ten Linux
ASan/UBSan CTest tests (including the native workflows) passed. For CMake, opt
in with `-DJOT_LINUX_E2E=ON`; the native test timeout is 120 seconds. The optional
suite requires `mutter`, `dbus-run-session`, Python PyGObject/Gio, and a working
headless Mutter/EGL setup (verified with Mutter 51.1). Use `make test-linux-e2e ARGS=--quick` to skip the large
note. Logs and fixture notes remain in `.build/linux/e2e/run-*` (CMake uses its
build directory's `e2e/`). The checks inspect saved documents, clipboard
transfers, process exits, and compositor protocol traffic; they do not compare
live screenshots or cover every GNOME Shell interaction or other compositor.
OCR is disabled in these native fixtures; the separate real-Tesseract test
continues to cover OCR.

The final normal-build large-note run measured 1,511 KiB idle PSS, with zero
process CPU ticks during the idle check. Image pixels are decoded for rendering
and released; 50 decoded images are not retained. These are editor-process
figures excluding the compositor and optional OCR children. They do not measure
transient decode peaks or represent 50 large screenshots. Sanitizer builds have
substantial instrumentation/quarantine overhead and are not footprint measures.
The empty-page target remains below 1 MiB: three fresh normal-build runs measured
742–747 KiB PSS (688–692 KiB private). The Linux executable is 789,208 bytes with
debug information or 192,376 bytes stripped; Windows remains 266,752 bytes.

## Typing around images (2026-11-02)

Normal rendering keeps image references on their own lines during ordinary text
insertion. Pasting an image in insert mode leaves the cursor after its reference,
so subsequent typing does not turn the image into literal Markdown. On a
selected image, `i`/`I` open a text line above and `a`/`A` open one below. Typing,
Tab, Enter, or text paste after moving onto an image in insert mode uses a
separate text line; normal-mode text puts use linewise placement there. Raw
mode retains literal source editing, and polish remains read-only.

Regression checks cover continuing after image paste, `i/I/a/A` from hidden
source offsets, direct insert-mode typing/Tab/Enter, text paste, grouped
undo/redo, actual inline-image rendering, raw editing, and save/reopen. All nine
Linux tests, nine Windows/Wine tests, and nine ASan/UBSan tests passed. Linux is
785,376 bytes with debug information or 192,376 bytes stripped; Windows remains
266,752 bytes. Three empty-page launches measured 762–765 KiB idle PSS
(692–696 KiB private), still below 1 MiB.

## Rounded client decorations (2026-11-02)

Floating Wayland windows with client-side decorations now have 12-logical-pixel
rounded corners. Maximized/fullscreen windows and windows reported as tiled
remain square. Server-side decoration behavior is unchanged, allowing Hyprland
and other compositors to supply their own rounding/shadows. The backend now
negotiates xdg-shell version 2 for tiled-edge states, with a version 1 fallback.
Windows continues to use its native window frame.

Only rounded windows use ARGB8888; other windows retain XRGB8888. Both buffers
use four bytes per pixel. The renderer supersamples the four corner squares
without another buffer or cache, and premultiplies their alpha. The backend
sends a conservative opaque region and curved input region, including a small
fringe for fractional scaling and resize targets. Regions are updated only when
window dimensions or corner radius change. There are no client-drawn shadows,
new runtime dependencies, extra redraws, or changes to the 30 FPS cap.

All nine Linux tests, nine Windows/Wine tests, and nine Linux ASan/UBSan tests
passed. Checks cover transparent/antialiased/premultiplied corners, pitched and
tiny buffers, square window states, and real Wayland region requests with
visible-pixel/input coverage and opaque-pixel assertions at 100/125/150/200%.
A light render preview was inspected. A native GNOME run accepted the ARGB
buffer and surface regions, with zero process CPU ticks over two idle seconds.
A separate 20,000-iteration corner-mask measurement at 150% averaged about
0.007 ms per frame on this host; this excludes the rest of painting.

The Linux executable is 784,328 bytes with debug information, or 192,376 bytes
stripped (4,096 bytes above the preceding image-cleanup build). Windows remains
266,752 bytes. The first three empty-page GNOME samples measured 736–740 KiB
idle PSS, versus 724–733 KiB immediately before the change. Code residency and
shared-library usage affect PSS; these measurements remain below 1 MiB. Final verification samples measured 735–743 KiB PSS. The
existing four-byte buffer sizes, two-buffer limit, and idle pixel unmapping are
unchanged. These are editor-process measurements, excluding compositor memory.

## Image-cleanup verification (2026-11-02)

All nine Linux Make tests, all nine Windows/Wine tests, and all nine Linux
AddressSanitizer/UndefinedBehaviorSanitizer tests passed with the cleanup change. Regression checks cover unsaved deletion/undo/discard, raw and
inline remaining references, failed saves, failed cleanup and retry, untracked
files, import/delete/save-and-quit, and stopping an active OCR job without
recreating sidecars. The Linux executable measured 773,448 bytes with debug
information, or 188,280 bytes stripped; Windows measured 266,752 bytes. Three
empty-page Wayland launches measured 742–750 KiB idle PSS, below 1 MiB.

## Verification

- Nine native Linux CMake tests passed, including a private Wayland protocol
  server exercising large nonblocking text transfer, plain-only paste, PNG
  copy, file/URI drop and DND finish. No real clipboard was replaced.
- Nine tests passed under Wine: real executable/window input, DPI/UTF-16,
  file-save behavior, shared modal/page/image tests and private clipboard
  Unicode/CRLF/PNG/DIB/file/fragment format tests. The clipboard store is mocked
  to isolate it; the format conversion and Win32 memory/window code are real.
- All nine Linux tests also passed with AddressSanitizer/UndefinedBehaviorSanitizer.
  A 30,000-action randomized modal stress run passed during development.
- An actual Tesseract child recognized a bundled-font image and produced text
  and TSV sidecars with 0600 permissions. A deterministic delayed child tested
  search retry, open-prompt updates and cancellation after another key.
- Tests cover cross-page mixed text/image copies, OCR phrase rectangle unions,
  headings skipping fenced code, word boundaries, UTF-8 find repeats, grouped
  insert/visual-put undo, safe saves, manual saves/reload and maximized preferences.
- CPU render previews were inspected at 12/16/18/22 logical pixel sizes and
  100/125/150/200% scales. Syntax stayed visible, conservative Markdown cases
  stayed plain, text wrapped correctly, and inline images fit the view.

`make page-preview` builds a reproducible, window-free preview utility:
`./.build/linux/tools/page-preview notes.md preview.ppm 18 150` (font size and scale in 120ths). Append `light` or `dark` for the palette, `raw`, `normal` or `polish` for the
render mode, and `decorated` for the Wayland client title bar.
Native Windows hardware testing and compositor-dependent activation behavior
remain environmental validation limits; see [windows.md](windows.md).
