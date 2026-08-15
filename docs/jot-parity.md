# Jot feature parity work

Reference: https://github.com/osigurdson/jot at
`4cdc1ccb336dab9237f97394b70bd8704c1758b3` (README, docs/keys.md, and source).
The implementation stays native C/Wayland on Linux and Win32/GDI on Windows,
using the current bundled outline font. GTK/libadwaita/Pango are not dependencies.

The comparison is behavioral: native presentation need not duplicate GTK pixels.
The reference itself excludes dot repeat, macros, marks, named registers, block
visual mode, general config/plugins, and exact Vim regex compatibility.

## Delivery checklist

- [ ] Normal/insert/visual/visual-line modes, counts, complete documented motions.
- [ ] Operators, text objects, case changes, joins, replace, indent/dedent.
- [ ] Bounded lazy undo/redo, one insert session per undo step.
- [ ] Incremental smart-case search, word search, next/back, match highlighting.
- [ ] Ex ranges, substitutions with captures, command history, documented commands.
- [ ] Default/named/path scratch pages, private folders, agent files, autosave.
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
