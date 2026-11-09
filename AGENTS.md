# mib

A small C notes editor with Vim-style modal editing and native Wayland and Windows backends.
README.md covers building and testing; docs/keys.md is the user-facing key reference.

## Layout

- `src/keys.c`: normal, visual and insert mode keys, motions, text objects, operators.
- `src/ex.c`, `src/substitute.c`: `:` commands. `src/history.c`: grouped undo/redo.
- `src/app.c`, `src/media.c`: app-level input (images, clipboard paste), shared by both platforms.
- `src/navigator.c`: the page list (`Alt+0`, `:ls`), its drawing (`navigator_draw`) and click
  hit-testing (`navigator_page_at`). It uses the `render_*` helpers and `struct palette` that
  `src/render.c` shares through `src/render-internal.h`. `src/help.c` draws the `:help` card the
  same way.
- `src/pages.c`: the nine pages `1/` to `9/` in the notes folder (`app.root`), each with
  `page.md` and `assets/`. The shown page lives in `app.editor`, `app.media` and
  `app.directory`; switching swaps the per-document editor fields (text, cursor, undo, marks)
  and media state with `app.pages[]`. `src/session.c` resolves the folder.
- `src/platform/linux/wayland.c` and `src/platform/windows/window.c` map keys to `enum editor_key`.
  A new Ctrl key needs an entry in `src/editor.h` and in both platform files.
- Every text change goes through `editor_raw_replace` in `src/history.c`, which also shifts
  marks and jump-list positions.
- The unnamed register is the system clipboard: yanks and deletes copy to it, and `p` pastes
  from it through the platform's `ED_PASTE` handler.

## Checks

- `make test`: unit and integration tests. Add modal behaviour tests to `tests/modal-test.c`.
- `make format-check`: C files follow `.clang-format`; run `make format` before committing.
  `make compile-commands` writes the clangd database (gitignored); `.inc` files are excluded.
- `make windows`: cross-compiles the Windows build; run it after touching shared or Windows code.
- `make demo`: re-records the README demos, `docs/demo.svg` and `docs/demo-ocr.svg` (needs
  Tesseract), by driving the real editor (`tools/demo-frames.c`) through the pages and keys at
  the top of `tools/demo.py`. Run it after visible UI changes and check the frames it keeps in
  `.build/linux/demo-frames/` and `.build/linux/demo-ocr-frames/`.
- `make fuzz`: libFuzzer (clang) on image files, page text drawn in every view and pasted
  fragments, for `FUZZ_TIME` seconds (default 60); run it after touching those parsers. Crashes
  land in `.build/linux/fuzz/` as `crash-*`; rerun one with `.build/linux/fuzz/mib-fuzz FILE`.
- `make vim-compare`: types each case in `tests/vim-compare.toml` into the real mib binary (in a
  private headless Mutter session) and into Vim, then compares the saved files.

## CI and releases

- `.github/workflows/ci.yml` runs on every push and pull request: `make`, `make test`,
  and `make format-check` (clang-format pinned to the local version through pipx), and
  uploads the Linux tarball it tested. Windows is not in CI yet; run `make test-windows`
  locally.
- Pushing a `v*` tag runs `release.yml`, which reuses that CI tarball, adds SHA256SUMS and
  build provenance attestations, creates the release and redeploys the download page.
- `site/index.html` is the download page; `tools/site.py OUT [--release JSON]` fills it with
  the latest release. `site/install.sh` is the `curl | sh` installer published beside it;
  test changes with `MIB_PREFIX=/tmp/x sh site/install.sh` against a real release. `tools/version.py` stamps `mib --version` with `git describe` and the
  commit.
- Actions are pinned by commit SHA with the version in a comment; keep it that way when
  updating them.

## Change workflow

For each change: implement it with the checks above passing, commit it on `main`, then run
`make install` so `~/.local/bin/mib` is the build the user tries. Restart mib to pick up a
new install; an open window keeps the old binary and owns its page.

## Vim parity workflow

Use `make vim-compare` when adding or changing Vim behaviour:

1. Add cases to `tests/vim-compare.toml` before implementing. Keys use Vim notation
   (`<Esc>`, `<CR>`, `<C-o>`, `<lt>`). Only file contents are compared, so end motion-only
   cases with an edit such as `x` to expose the cursor position.
2. New cases that don't match yet get `status = "gap"`. `ARGS=--vim` prints what Vim produces,
   which helps check that a case tests what you mean.
3. Implement, then run `make vim-compare ARGS="--only REGEX"` while iterating and the full run at
   the end. A gap that now matches is reported as FIXED and fails the run until you remove its
   `status` line; a passing case that stops matching is a FAIL with a diff.
4. The Vim reference runs `vim -u NONE -es` with mib's conventions (4-space `expandtab`,
   `autoindent`, `ignorecase smartcase`, `nojoinspaces`, `textwidth=79`). If a difference is a
   deliberate mib choice, keep it as a gap with a `note`.
5. `:normal` differs from typing in a few ways: it drops the remaining keys after a failing
   command (typed Vim beeps and continues), `u` undoes everything since `:normal` started, and
   `q` can't record. Give such cases a `vim_keys` field with what interactive Vim effectively
   runs. A case that passes because both sides did nothing is worth a second look.

Failing runs keep each case's mib file, Vim file and client log under
`.build/linux/vim-compare/run-*/`.

The `status = "gap"` cases double as the list of known missing Vim features (macros, visual
block mode, `Ctrl-A`, insert counts, `:g`/`:sort`/`:m`/`:t`, insert-mode `Ctrl-R`/`Ctrl-O`,
and `G` landing on the empty line after a final line break).
