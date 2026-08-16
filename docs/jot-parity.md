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
  highlighted text and OCR word/phrase rectangles. A pending failed search
  retries when OCR completes; another editing/navigation key cancels the retry.
- Ex ranges, capture substitutions, command history, save/reload/quit,
  line numbers, Markdown toggles, folder opening and zoom.
- Default/named/path scratch pages, private Linux folders, agent instructions
  created only for fresh pages, explicit saves, focus reload,
  persisted dimensions/maximized/font/options and one owner per page.
- Native text/image clipboard, cut/yank/put and visual replacement, PNG/JPEG
  file import, URI/file drops, inline images, image/link opening and image
  Open/Copy Text context actions. Image copy also offers plain Markdown text.
- Mixed text/image copies between pages import new asset files using an
  internal clipboard fragment format. Original assets remain untouched.
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
`./.build/linux/tools/page-preview notes.md preview.ppm 18 150` (font size and scale in 120ths).
Native Windows hardware testing and compositor-dependent activation behavior
remain environmental validation limits; see [windows.md](windows.md).
