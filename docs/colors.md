# Colors

Jot uses Vim-style commands:

```vim
:set background=dark
:set background=light
:set bg=dark
:set background?
:colorscheme desert
:colo peachpuff
:colorscheme
```

Schemes: `jot`, `default`, `desert`, `evening`, `peachpuff`.
The former `:set light` and `:set dark` commands have been removed.
Settings are remembered per page, independently of saving the note.
Old preference files retain their original Jot palette and light/dark setting.

`jot` and `default` have light/dark variants. The classic `desert` and `evening`
palettes are dark; `peachpuff` is light. Selecting one of these classic schemes
sets its preferred background option. Changing the option afterwards keeps its
fixed palette, just as a Vim scheme can keep its own background colors.
`background` is an option describing the intended background, not a command to
rewrite a scheme's colors. See [Vim's background documentation](https://github.com/vim/vim/blob/master/runtime/doc/options.txt)
and [colorscheme documentation](https://github.com/vim/vim/blob/master/runtime/doc/syntax.txt).

## Palette mapping

The original `jot` palette is unchanged. The others map Vim 9.2 GUI highlight
colors from the installed upstream runtime (desert 2026-08-20, evening
2026-04-24, peachpuff 2026-04-24, default 2023-08-10). Source:
[vim/colorschemes](https://github.com/vim/colorschemes) and
[Vim's default highlights](https://github.com/vim/vim/tree/master/runtime/syntax).
The upstream license is included in [vim-LICENSE](../third_party/vim-LICENSE).

| Jot role | Vim highlight |
|---|---|
| Body | Normal |
| Gutter / markers | LineNr |
| Link | Underlined |
| Heading | Title |
| Quote / completed task | Comment |
| Italic | Special |
| Code background | CursorLine (subtle Normal blend if unset) |
| Search / selection | Search / Visual, including foreground colors |
| Status bar | StatusLine |
| Context menu | Pmenu / PmenuSel |
| Cursor | Cursor |

Window controls have Jot-specific colors derived from Normal. Heading sizes,
Markdown styling and the renderer remain Jot's; this does not load or execute
arbitrary Vimscript colorscheme files.

The seven palettes are fixed read-only tables of 25 32-bit colors (700 bytes
in total). Choosing a theme adds no per-document allocations, render buffers,
or font caches. The selected scheme is stored by name in `.tinyvi-state`.
