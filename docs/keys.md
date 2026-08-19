# Jot key reference

Raw and normal modes show line numbers. The gutter reserves four digits and grows
for longer notes, with two character cells between the numbers and text. Scratch
pages open in normal mode with the cursor at the end; explicit files start at
the beginning. The bottom line shows the line and column, plus `RAW` or
`POLISH [read only]` outside normal rendering, and messages.

Counts work with motions and operators, such as `3w`, `5j`, `d3j` and `2dd`.

An unsupported command is discarded with a message, so the keys that follow it
run as normal commands. Macros (`q`, `@`) aren't supported.

## Modes

| Keys | Action |
|---|---|
| `i` `a` `I` `A` | Insert at the cursor, after it, at the line's start, or at its end |
| `o` `O` | Open a line below or above |
| `s` `S` | Replace characters or the whole line |
| `v` `V` | Visual or visual line mode |
| `Esc`, `Ctrl+[` | Back to normal mode |

## Moving

| Keys | Action |
|---|---|
| `h` `j` `k` `l`, arrows | Left, down, up, right |
| `w` `b` `e`, `W` `B` `E` | By word, or by space-separated word |
| `0` `^` `$` `g_` | Line start, first non-blank, line end, last non-blank |
| `gg` `G` `{n}G` | First line, last line, line n |
| `f` `F` `t` `T`, `;` `,` | To a character on the line; repeat, repeat backward |
| `{` `}` | Previous or next blank line |
| `%` | Matching bracket |
| `H` `M` `L` | Top, middle or bottom of the window |
| `]]` `[[` | Next or previous Markdown heading |
| `Ctrl+D` `Ctrl+U` | Half a page down or up |
| `Ctrl+F` `Ctrl+B`, PgDn, PgUp | A page down or up |
| `Ctrl+E` `Ctrl+Y` | Scroll without moving the cursor |
| `zz` `zt` `zb` | Put the cursor line in the middle, at the top or at the bottom |

## Editing

| Keys | Action |
|---|---|
| `d` `y` `c` `<` `>` | Delete, yank, change, dedent, indent: follow with a motion or text object, such as `dw`, `ci"` or `yip` |
| `dd` `yy` `cc` `>>` `<<` | The same, on whole lines |
| `x` `X` | Delete a character forward or backward |
| `D` `C` `Y` | Delete to the line end, change to the line end, yank the line |
| `p` `P` | Put after or before the cursor |
| `J` | Join lines |
| `r` | Replace a character |
| `~` | Toggle case |
| `gu` `gU` `g~` | Lowercase, uppercase or toggle case: follow with a motion or text object, or double the last key (`gUU`) for the line |
| `u` `Ctrl+R` | Undo, redo (one insert session is one undo step) |

In visual mode, `d` `y` `c` `<` `>` act on the selection, `J` joins, `u` `U` `~` change case, `p` replaces, and `o` jumps to the other end.

## Text objects

Use these after an operator or in visual mode. `i` means inside and `a` includes the surroundings.

| Keys | Selects |
|---|---|
| `iw` `aw` `iW` `aW` | A word |
| `i"` `a"` `i'` `a'` | Inside or around quotes |
| ``i` `` ``a` `` | Inside or around backticks |
| `i(` `a(` `ib` `ab` | Parentheses |
| `i[` `a[` | Square brackets |
| `i{` `a{` `iB` `aB` | Braces |
| `i<` `a<` | Angle brackets |
| `ip` `ap` | A paragraph |

## Search

| Keys | Action |
|---|---|
| `/` `?` | Search forward or backward, updating as you type |
| `n` `N` | Next or previous match |
| `*` `#` | Search for the word under the cursor |

Entering insert mode hides search highlights without clearing the query.
Returning to normal mode keeps them hidden until `n`/`N`, `*`/`#`, or a new
`/`/`?` search. Search is case-insensitive unless your search contains a capital letter. It also finds text inside screenshots using Tesseract-generated OCR sidecars.

## Images and links

An image is selected when the cursor is on it. Move onto its line or click it.

| Keys | Action |
|---|---|
| `i` `I`, `a` `A` | In normal rendering, insert text on a new line above or below the image |
| `gx`, double-click | Open the image in your image viewer |
| `gx`, `Ctrl`+click | Open the link under the cursor |
| `yy` | Copy the image, ready to paste into other apps |
| `d`, `x` | Delete the selected image in normal mode |
| `Delete` | Delete the selected image in normal or insert mode |
| `Backspace`, `Ctrl+W`, `Ctrl+U` | In insert mode, delete a selected image as a whole block |
| `p`, `Ctrl+V` | Paste a screenshot from the clipboard |
| `Ctrl+Shift+C` | Copy the image's text |

In normal rendering, typed and pasted text stays on a separate line from an
image. Pasting an image in insert mode places the cursor after it so you can
continue typing. In insert mode, `d` types text like any other letter. Raw mode
still edits the literal Markdown source. In normal rendering, Backspace from the
start of text after an image and Delete from the end of text before an image
remove that image block, preserving the neighboring text and line boundary.

Deleted image files and OCR sidecars are removed only after a successful save,
provided their asset paths no longer appear in the note. Before saving, undo and
quitting without saving retain the files. A save that cleans up images resets
undo/redo history and clears a local register that refers to a removed image.
Ordinary saves keep history. Clipboard references to removed files can expire.

Right-clicking an image also offers **Copy Text from Image** and **Open Image**.

## Insert mode

| Keys | Action |
|---|---|
| `Home`, `End` | Move to the line start or the insertion point after its last character |
| `Ctrl+W` | Delete the previous word |
| `Ctrl+U` | Delete back to the line's start |
| `Ctrl+V` | Paste text or a screenshot |
| `Tab`, `Shift+Tab` | Indent, dedent |
| `Enter` | New line, keeping the indentation |

## Command line

Press `:` to open it. Up and Down browse your history.

| Command | Action |
|---|---|
| `:w` | Save the current page or file |
| `:q`, `:wq`, `:x` | Close the window |
| `:{n}`, `:$` | Go to line n, or the last line |
| `:[range]s/pat/rep/[gi]` | Substitute, with `\( \) \| \+ \< \>`, `&` and `\1` |
| `:[range]d`, `y`, `>`, `<`, `j` | Delete, yank, indent, dedent or join lines |
| `:set raw` | Editable plain source, with no Markdown styling or inline images |
| `:set normal` | Editable styled source with inline images (default) |
| `:set polish` | Read-only reading view; conceal supported markers and hide the gutter |
| `:set background=light`, `:set background=dark` | Set the background option (`bg` is an alias) |
| `:set background?` | Show the background setting |
| `:colorscheme {name}`, `:colo {name}` | Choose jot, tokyonight, catppuccin, kanagawa, or everforest |
| `:colorscheme` | Show the active scheme |
| `:Ex` | Open the page's folder in your file manager |
| `:e!` | Reload the page from disk |
| `:u`, `:redo` | Undo, redo |

Ranges can use `%`, `.`, `$`, line numbers, `'<,'>` and `+n` or `-n`.

Render mode, background and colorscheme are remembered per scratch page. In polish, movement,
search, visual selection, copying, saving and link/image opening remain available.
Typing, paste, drops, deletion, substitutions, undo and redo are blocked. Switch
to raw or normal to edit. Copies retain the selected Markdown source.

## Window

| Keys | Action |
|---|---|
| `Ctrl+S` | Save now |
| `Ctrl+Q` | Quit |
| `ZZ`, `ZQ` | Save and close, or close without saving |
| `Ctrl +`, `Ctrl -`, `Ctrl 0` | Zoom in, zoom out, reset |

On Wayland, a title bar appears when the compositor needs client-side window
controls. Drag it to move the window, double-click it to maximize/restore, or
use its minimize, maximize and close buttons. Drag an edge or corner to resize;
right-click the title for the compositor's window menu. Floating windows with
client-side controls have rounded corners. They become square when maximized,
tiled, or fullscreen; server-decorated windows follow the compositor's styling.

This feature set follows [jot](https://github.com/osigurdson/jot), pinned in the
[parity report](jot-parity.md). Native presentation differs. Search selects an
image containing OCR matches and highlights matching OCR words. The font subset,
ASCII case handling and bounded regex subset are intentional limitations.
Pages and explicit files both protect unsaved changes on quit. Use `:w` or
`Ctrl+S` to save, `:wq` to save and quit, or `:q!` to discard edits.
