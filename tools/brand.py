#!/usr/bin/env python3
"""Writes mib's brand assets from one set of geometry.

The wordmark is drawn as round-capped strokes, so the letters stay consistent at every size:
an arched "m", an "i" with a round dot, a "b" whose bowl meets its stem, and the orange bar
underneath that reads as a text cursor. Run `python3 tools/brand.py` from the repository root.
"""
import os
import subprocess
import sys

INK = "#100E14"  # near-black background
CREAM = "#FDF4E4"
ORANGE = "#FF9F0A"

STROKE = 36  # letter stroke width in wordmark units
BAR = 30  # bar stroke width

# Letter centrelines. Baseline caps end at y=138, the x-height at y=12, the ascender at y=-68.
M = "M20 120 V60 A30 30 0 0 1 80 60 V120 M80 60 A30 30 0 0 1 140 60 V120"
I_STEM = "M196 30 V120"
I_DOT = (196, -30, 21)
B = "M256 -50 V120 M256 75 A45 45 0 1 0 346 75 A45 45 0 1 0 256 75"
WORD = (2, -68, 364, 138)  # outer bounds of the letters


def letters(colour, with_i=True, with_b=True):
    s = f'<g fill="none" stroke="{colour}" stroke-width="{STROKE}" stroke-linecap="round" stroke-linejoin="round">'
    s += f'<path d="{M}"/>'
    if with_i:
        s += f'<path d="{I_STEM}"/>'
    if with_b:
        s += f'<path d="{B}"/>'
    s += "</g>"
    if with_i:
        x, y, r = I_DOT
        s += f'<circle cx="{x}" cy="{y}" r="{r}" fill="{colour}"/>'
    return s


def bar(colour, x0, x1, y):
    return (f'<path d="M{x0} {y} H{x1}" stroke="{colour}" stroke-width="{BAR}" '
            f'stroke-linecap="round"/>')


def wordmark(text_colour, bar_colour):
    """The full mark: letters plus the bar centred under them. Bounds 2..364 x -68..195."""
    centre = (WORD[0] + WORD[2]) / 2
    half = 113
    return letters(text_colour) + bar(bar_colour, centre - half, centre + half, 180)


# A wider m for 16-32 pixel sizes, so its two counters stay open.
M_SMALL = "M20 120 V62 A34 34 0 0 1 88 62 V120 M88 62 A34 34 0 0 1 156 62 V120"


def small_mark(text_colour, bar_colour):
    """Just the wide m and its bar. Bounds 2..174 x 10..195."""
    return (f'<path d="{M_SMALL}" fill="none" stroke="{text_colour}" stroke-width="{STROKE}" '
            f'stroke-linecap="round" stroke-linejoin="round"/>' + bar(bar_colour, 30, 146, 180))


FONT = "/usr/share/fonts/TTF/JetBrainsMonoNerdFontMono-Bold.ttf"


def text_path(text, colour, size, x, baseline):
    """Text as outlines in JetBrains Mono Bold, so it renders the same without the font.
    Returns the SVG and the advance width."""
    from fontTools.pens.svgPathPen import SVGPathPen
    from fontTools.ttLib import TTFont

    font = TTFont(FONT)
    glyphs = font.getGlyphSet()
    cmap = font.getBestCmap()
    scale = size / font["head"].unitsPerEm
    out, pen_x = "", 0
    for ch in text:
        name = cmap[ord(ch)]
        pen = SVGPathPen(glyphs)
        glyphs[name].draw(pen)
        d = pen.getCommands()
        if d:
            out += (f'<path transform="translate({x + pen_x * scale:.2f} {baseline}) '
                    f'scale({scale:.5f} {-scale:.5f})" d="{d}"/>')
        pen_x += glyphs[name].width
    return f'<g fill="{colour}">{out}</g>', pen_x * scale


def svg(width, height, body, title="mib"):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
            f'viewBox="0 0 {width} {height}"><title>{title}</title>{body}</svg>\n')


def lockup(text_colour, title):
    """Wordmark over the tagline, on a transparent background, for the README header."""
    tagline, width = text_path("Markdown under a mebibyte.", text_colour, 28, 0, 0)
    pad, scale = 8, 0.8
    mark_height = 263 * scale  # ascender to the bottom of the bar
    baseline = pad + mark_height + 52
    total = max(362 * scale, width) + 2 * pad
    mark_x = (total - 362 * scale) / 2
    body = (f'<g transform="translate({mark_x - 2 * scale:.2f} {68 * scale + pad:.2f}) '
            f'scale({scale})">{wordmark(text_colour, ORANGE)}</g>'
            f'<g transform="translate({(total - width) / 2:.2f} {baseline:.2f})">{tagline}</g>')
    return svg(round(total), round(baseline + 8 + pad), body, title)


def badge():
    """mib | < 1 MiB, for the top of the README and release notes."""
    h, r = 56, 14
    scale = 0.15
    left = 362 * scale + 32
    label, width = text_path("< 1 MiB", INK, 26, 0, 0)
    right = width + 32
    w = left + right
    body = (f'<clipPath id="c"><rect width="{w:.2f}" height="{h}" rx="{r}"/></clipPath>'
            f'<g clip-path="url(#c)"><rect width="{w:.2f}" height="{h}" fill="{INK}"/>'
            f'<rect x="{left:.2f}" width="{right:.2f}" height="{h}" fill="{ORANGE}"/></g>'
            f'<g transform="translate({16 - 2 * scale:.2f} {(h - 263 * scale) / 2 + 68 * scale:.2f}) '
            f'scale({scale})">{wordmark(CREAM, ORANGE)}</g>'
            f'<g transform="translate({left + 16:.2f} {h / 2 + 9:.2f})">{label}</g>')
    return svg(round(w), h, body, "mib: under 1 MiB")


def tile(content, bounds, background, border, scale, size=512, radius=114, border_width=0):
    x0, y0, x1, y1 = bounds
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    inset = border_width / 2
    s = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{size}" height="{size}" '
         f'viewBox="0 0 512 512"><title>mib</title>')
    s += f'<rect width="512" height="512" rx="{radius}" fill="{background}"/>'
    if border_width:
        s += (f'<rect x="{inset}" y="{inset}" width="{512 - border_width}" '
              f'height="{512 - border_width}" rx="{radius - inset}" fill="none" '
              f'stroke="{border}" stroke-width="{border_width}"/>')
    s += f'<g transform="translate(256 256) scale({scale}) translate({-cx} {-cy})">'
    s += content + "</g></svg>\n"
    return s


def windows_icon(full_svg, small_svg, path, scratch):
    """A multi-size .ico: the small m up to 32 pixels, the full wordmark above that."""
    pngs = []
    for size in (16, 20, 24, 32, 48, 64, 128, 256):
        source = small_svg if size <= 32 else full_svg
        png = os.path.join(scratch, f"icon-{size}.png")
        subprocess.run(["rsvg-convert", "-w", str(size), "-h", str(size), source, "-o", png],
                       check=True)
        pngs.append(png)
    # Pillow stores each size as PNG, which keeps the file small enough to embed.
    from PIL import Image

    images = [Image.open(png) for png in pngs]
    images[-1].save(path, sizes=[i.size for i in images], append_images=images[:-1])


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "docs/brand"
    os.makedirs(out, exist_ok=True)
    full = (WORD[0], WORD[1], WORD[2], 195)
    m_only = (2, 10, 174, 195)
    files = {
        "icon.svg": tile(wordmark(CREAM, ORANGE), full, INK, CREAM, 1.0, border_width=14),
        "icon-light.svg": tile(wordmark(INK, ORANGE), full, CREAM, CREAM, 1.0),
        "icon-small.svg": tile(small_mark(CREAM, ORANGE), m_only, INK, CREAM, 1.7),
        "wordmark-dark.svg": lockup(CREAM, "mib: Markdown under a mebibyte"),
        "wordmark-light.svg": lockup(INK, "mib: Markdown under a mebibyte"),
        "badge.svg": badge(),
    }
    for name, svg in files.items():
        with open(os.path.join(out, name), "w") as f:
            f.write(svg)
    print("wrote", ", ".join(files), "to", out)
    if len(sys.argv) > 1:
        return  # a preview run; leave the packaged icons alone
    with open("packaging/dev.jot.Jot.svg", "w") as f:
        f.write(files["icon.svg"])
    import tempfile

    with tempfile.TemporaryDirectory() as scratch:
        windows_icon(os.path.join(out, "icon.svg"), os.path.join(out, "icon-small.svg"),
                     "packaging/mib.ico", scratch)
    print("wrote packaging/dev.jot.Jot.svg and packaging/mib.ico")


if __name__ == "__main__":
    main()
