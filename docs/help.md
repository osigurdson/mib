# mib help

Vim keys for Markdown notes. This page is read-only: move and search it as usual, and press `Esc` to go back to your page.

## Pages

- `Alt+1` … `Alt+9`: show page 1 to 9
- `Alt+0`, `:ls`: list the pages; `:b N` shows page N
- `:w`, `Ctrl+S`: save the page; `:wa` saves every page
- `:q`, `Ctrl+Q`: close the window; unsaved changes on any page stop it

Each page is a `page.md` with its own `assets/` folder, in the notes folder (`~/Documents/mib/1` to `9` by default).

## Unlike Vim

- `y` `d` `p`: use the system clipboard; named registers `"a` to `"z` still work
- `Ctrl+V`: pastes text or a screenshot; there is no block visual mode
- `:set wrapwidth=N`: soft-wrap at column N (80 by default, 0 for the window edge); typing never adds line breaks (no `textwidth`)
- `gq`: hard-wrap lines at 79 columns
- `:colo <Tab>`: try the color schemes live; `Enter` keeps one, `Esc` goes back

Macros, `Ctrl-A`, `:g`, `:sort` and insert-mode `Ctrl-R` aren't supported yet.

## Views

- `:set view=normal`: styled Markdown with inline images (default)
- `:set view=source`: plain source
- `:set view=reading`: a read-only reading view

## Images and links

- `p`, `Ctrl+V`: paste a screenshot; dropping an image file adds it too
- `/`: search also finds text inside screenshots (with Tesseract installed)
- `+` `-` `=`: resize the selected image, or reset it
- `gx`, `Ctrl`+click: open the link or image under the cursor
- Right-click an image: copy it, copy its text, or open it

## More

Every key and command: https://github.com/osigurdson/mib/blob/main/docs/keys.md

`Ctrl`+click the link, or put the cursor on it and type `gx`, to open it in your browser.
