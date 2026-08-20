# Jot

A small C notes editor with Vim-style modal editing and native Wayland and Windows backends.
README.md covers building and testing; docs/keys.md is the user-facing key reference.

## Layout

- `src/keys.c`: normal, visual and insert mode keys, motions, text objects, operators.
- `src/ex.c`, `src/substitute.c`: `:` commands. `src/history.c`: grouped undo/redo.
- `src/app.c`, `src/media.c`: app-level input (images, clipboard paste), shared by both platforms.
- `src/platform/linux/wayland.c` and `src/platform/windows/window.c` map keys to `enum editor_key`.
  A new Ctrl key needs an entry in `src/editor.h` and in both platform files.
- Every text change goes through `editor_raw_replace` in `src/history.c`, which also shifts
  marks and jump-list positions.
- The unnamed register is the system clipboard: yanks and deletes copy to it, and `p` pastes
  from it through the platform's `ED_PASTE` handler.

## Checks

- `make test`: unit and integration tests. Add modal behaviour tests to `tests/modal-test.c`.
- `make windows`: cross-compiles the Windows build; run it after touching shared or Windows code.
- `make vim-compare`: types each case in `tests/vim-compare.toml` into the real Jot binary (in a
  private headless Mutter session) and into Vim, then compares the saved files.

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
4. The Vim reference runs `vim -u NONE -es` with Jot's conventions (4-space `expandtab`,
   `autoindent`, `ignorecase smartcase`, `nojoinspaces`, `textwidth=79`). If a difference is a
   deliberate Jot choice, keep it as a gap with a `note`.
5. `:normal` differs from typing in a few ways: it drops the remaining keys after a failing
   command (typed Vim beeps and continues), `u` undoes everything since `:normal` started, and
   `q` can't record. Give such cases a `vim_keys` field with what interactive Vim effectively
   runs. A case that passes because both sides did nothing is worth a second look.

Failing runs keep each case's Jot file, Vim file and client log under
`.build/linux/vim-compare/run-*/`.

The `status = "gap"` cases double as the list of known missing Vim features (macros, visual
block mode, `R`, `Ctrl-A`, insert counts, `:g`/`:sort`/`:m`/`:t`, insert-mode `Ctrl-R`/`Ctrl-O`,
and `G` landing on the empty line after a final line break).
