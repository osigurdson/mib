# Jot feature parity work

Reference: https://github.com/osigurdson/jot at
`4cdc1ccb336dab9237f97394b70bd8704c1758b3` (README, docs/keys.md, and source).
The implementation stays native C/Wayland on Linux and Win32/GDI on Windows,
using the current bundled outline font. GTK/libadwaita/Pango are not dependencies.

The comparison is behavioral: native presentation need not duplicate GTK pixels.
The reference itself excludes dot repeat, macros, marks, named registers, block
visual mode, general config/plugins, and exact Vim regex compatibility.

## Delivery checklist

- [x] Normal/insert/visual/visual-line modes, counts, complete documented motions.
- [x] Operators, text objects, case changes, joins, replace, indent/dedent.
- [x] Bounded lazy undo/redo, one insert session per undo step.
- [x] Incremental smart-case search, word search, next/back, match highlighting.
- [x] Ex ranges, substitutions with captures, command history, documented commands.
- [x] Default/named/path scratch pages, private folders, agent files, autosave.
- [ ] External-edit reload on focus, persisted geometry/options, single page ownership.
- [ ] System text/image clipboard, paste/copy/cut, image files, drag/drop.
- [ ] Inline image rendering/selection/open/delete, optional OCR text and TSV search.
- [ ] Markdown styles/links/headings, line numbers, wrapping, scrolling and zoom.
- [ ] Native keyboard/mouse integration and regression tests on Linux and Wine.
- [ ] Measured binary sizes and idle/image memory; no eager image/history allocation.

Existing explicit-file invocation and safe manual save behavior remain available.
Scratch-page invocation follows jot's autosaved-page flow. Automated tests use
isolated temporary pages, never the user's real jot data or system clipboard.

## Size and memory discipline

Baseline before this work: Linux debug executable 161,056 bytes; idle empty-file
samples 2,768–2,832 KiB RSS and 669–688 KiB PSS on the current Wayland host.
PSS depends on shared-library usage by other applications. Source features and
images will grow the program; track release/stripped sizes separately from debug
symbols. Keep the executable below 1 MiB, with an empty-page idle PSS target of
1 MiB on this host. Undo, image pixels, OCR and clipboard storage must be lazy
and bounded; no resident OCR/font engine or heavyweight widget framework.

Implementation progress and exact verification results will be updated with
each feature group rather than calling a partial port complete.

## Editing and page foundation

Implemented a shared modal engine, bounded delta undo (2 MiB of payload, 1,024
records maximum, allocated lazily), smart-case incremental text search, and a
bounded Vim-style substitution VM. Regex intentionally supports a subset:
literals, dot, anchors, classes/ranges, `*`, `\+`, `\?`, `\(groups\)`, `\|`,
`\<`/`\>`, replacement `&`/`\1`–`\9`; no lookaround or full Vim regex syntax.
Regex work/backtracking and substitution output have explicit limits. Unicode
navigation preserves UTF-8 character boundaries; case conversion is ASCII.

Pages have private Linux directories, agent instructions created only for fresh
pages, 400 ms debounced autosave, final-newline serialization, external-change
reload on focus, saved dimensions/font/options, and one owner per page. Linux
activation relays XDG_ACTIVATION_TOKEN when supplied by the launcher (the
compositor decides whether to raise it). Windows uses a named page mutex.
Explicit file arguments retain manual saves; `--file`/`--page` remove ambiguity.
A filename extension selects explicit-file mode for compatibility with earlier
versions; otherwise arguments follow jot's page naming rules.

Interim modal/UI build: stripped Linux 93,248 bytes; Windows 127,488 bytes.
Empty-file PSS 697 KiB and RSS 2,796 KiB (two samples). These figures precede
scratch-page and image/clipboard additions. Native tests cover modal operations,
captures/ranges, large grouped insertion, page ownership/reload and permissions.

## Images and native clipboard checkpoint

Implemented nonblocking Wayland data-device transfers (text, PNG, URI lists and
file drops), Win32 Unicode/PNG/DIB/file clipboard formats, image imports,
standalone inline image blocks, image/link opening, and image context actions.
Image pixels are decoded for painting and released afterward. No decoded image
cache is retained at idle. stb is pinned and vendored under `third_party/stb/`;
PNG/JPEG decoding and PNG writing introduce no new runtime library dependency.

Bounds: 256 image references per page, 8 megapixels per decoded image, 32 MiB
encoded image/clipboard transfer, 16 MiB pasted text, 64 MiB total concurrent
codec allocations, 2 MiB per OCR sidecar read. Images larger than the viewport
fit within it. Additional image references remain editable Markdown text. OCR
runs one optional Tesseract child at a time; its memory is separate from the
editor. `TINYVI_OCR=0` disables jobs. Existing `.txt`/`.tsv` sidecars are usable
without Tesseract. Search currently visits each matching image as one result,
with matching words highlighted using TSV coordinates.

Checkpoint measurements: stripped Linux executable 155,192 bytes (baseline
60,416 bytes), Windows release executable 239,616 bytes. Empty-file idle samples
measured 785 KiB PSS / 2,928 KiB RSS versus baseline 669–688 KiB PSS /
2,768–2,832 KiB RSS. These are editor-process idle figures, not compositor/OCR
memory or transient image decode peaks. Both executables remain below 1 MiB.

Validation so far: Linux tests, eight Wine tests, ASan/UBSan media and modal
checks, and a 30,000-action randomized modal stress run passed. Rendered previews
were inspected at multiple sizes/scales; text wrapping, Markdown colors and
inline image sizing looked correct. `tools/page-preview.c` reproduces file
previews without launching a window.

Still to finish before calling this a complete port:
- Isolated end-to-end clipboard/data-device and drag/drop checks (without
  replacing the user's real clipboard), including image copy after deletion.
- Actual Tesseract subprocess/TSV round-trip and image idle memory measurement.
- Final comparison against the upstream key tests and correction of edge cases.
- Persist/restore maximized state and refine page activation/window titles.
- Final clean build, dependency/size measurements and documentation audit.
