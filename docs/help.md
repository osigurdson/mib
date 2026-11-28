## Pages
- `:ls` lists pages (first # H1 titles the page)
- `Alt+1` ... `Alt+9`: show page 1 to 9

## Views
- `:set view=normal`: styled Markdown with inline images (default)
- `:set view=source`: plain source
- `:set view=reading`: a read-only reading view

## Unlike Vim
- Soft-wraps at 80 chars by default ([wrapwidth](https://github.com/osigurdson/mib/blob/main/docs/keys.md#command-line))
- `y` `d` `p`: use the system clipboard

## Images
- `+` `-` `=` on an image: make it wider or narrower, or reset its size
- `y` on an image: copy it, ready to paste into other apps
- `gx`, double-click: open the image in your image viewer

## Updating
- To update mib, run the install command again: `curl -fsSL https://osigurdson.github.io/mib/install.sh | sh`
- That is all it takes. It installs the latest release over the old one and checks its checksum first.

## More
Every key and command: https://github.com/osigurdson/mib/blob/main/docs/keys.md
