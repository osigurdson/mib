# Colors

```vim
:colorscheme jot
:colorscheme tokyonight
:colorscheme catppuccin
:colorscheme kanagawa
:colorscheme everforest
:set background=dark
:set background=light
```

All five schemes follow `background`; `bg` is an abbreviation. Selecting a scheme
preserves the background setting. Use `:colorscheme` (`:colo`) to show the current
scheme and `:set background?` to show the background option.

| Scheme | Dark variant | Light variant |
|---|---|---|
| jot (default) | Original Jot dark | Original Jot light |
| tokyonight | Night | Day |
| catppuccin | Mocha | Latte |
| kanagawa | Wave | Lotus |
| everforest | Medium contrast | Medium contrast |

Settings are remembered per page, independently of saving the note. The Jot
palettes are unchanged. Pages using a removed scheme (`default`, `desert`,
`evening`, `peachpuff`) fall back to Jot, preserving their background setting.

## Upstream colors and licenses

These are compiled color tables adapted to Jot's existing Markdown and native UI
roles. No Lua, Vimscript, plugin code, or theme files are loaded at runtime.

| Theme | Pinned source | License |
|---|---|---|
| Tokyo Night, by folke | [tokyonight.nvim](https://github.com/folke/tokyonight.nvim/tree/cdc07ac78467a233fd62c493de29a17e0cf2b2b6) | [Apache 2.0](../third_party/tokyonight-LICENSE) |
| Catppuccin | [catppuccin/nvim](https://github.com/catppuccin/nvim/tree/edefef779ab08ce1a4a404713e3012b0d202bd35) | [MIT](../third_party/catppuccin-LICENSE) |
| Kanagawa, by Tommaso Laurenzi | [kanagawa.nvim](https://github.com/rebelot/kanagawa.nvim/tree/bb85e4bfc8d89b0e62c8fa53ccdd13d12e2f77b3) | [MIT](../third_party/kanagawa-LICENSE) |
| Everforest, by sainnhe | [everforest](https://github.com/sainnhe/everforest/tree/85a86eb62409e3ec88713bff3d1b9d7374e112e4) | [MIT](../third_party/everforest-LICENSE) |

| Jot role | Upstream highlight |
|---|---|
| Body | Normal |
| Gutter / markers | LineNr |
| Link | markdownLinkText / @markup.link / @markup.link.url / Underlined |
| Heading | Title |
| Quote / completed task | Comment |
| Italic | Special |
| Code background | CursorLine (subtle Normal blend if unset) |
| Search / selection | Search / Visual, including foreground colors |
| Status bar | StatusLine |
| Context menu | Pmenu / PmenuSel |
| Cursor | Cursor |

Window controls have Jot-specific colors derived from Normal. Heading sizes,
Markdown styling and the renderer remain Jot's. The Jot palettes retain all their
original colors.

## Footprint and regeneration

The ten palettes are fixed read-only tables of 25 32-bit colors: **1,000 bytes**
total. Choosing a theme adds no per-document allocations, render buffers, or font
caches. The selected scheme is stored by name in `.tinyvi-state`.

`tools/import-colors.py` exports upstream highlights through an isolated headless
Neovim process and generates `src/theme-palettes.h`. To regenerate, check out the
four sources above at their pinned revisions in folders named `tokyonight`,
`catppuccin`, `kanagawa`, and `everforest`, then run:

```sh
python3 tools/import-colors.py /path/to/checkouts
```

The importer was verified with Neovim 0.12.6. Its cache/data/state directories are
temporary and isolated. Normal builds use the checked-in header, with no import
step, network access, Neovim, or Lua dependency.
