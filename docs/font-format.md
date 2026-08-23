# Jot font format (TVF1)

TVF1 is a deliberately restricted, little-endian outline subset. The 19,164-byte
bundled file contains 134 glyphs from one static JetBrains Mono Nerd Font Mono
Regular TTF. It is embedded as read-only bytes in `jot`; the runtime never
opens the TTF or any system font. The regenerated derivative is named **Jot Mono
Subset**, with upstream copyright and licensing retained in `fonts/`.

## Build-time conversion

`tools/subset-font.py` uses FontTools to read a static TTF/OTF, resolve composite
transforms and implied on-curve points, and retain only explicit codepoint
mappings, monospace advance, vertical metrics, and outlines. Cubic CFF segments
are converted to quadratics with at most 0.5 design-unit approximation error.
Coordinates are rounded to signed 16-bit design units. Variable fonts and
non-monospace selected glyphs are rejected. No hints, layout, kerning, color,
variation, or original font tables survive the conversion.

`make subset-font` regenerates the binary and its JSON manifest from the bundled
TTF; this requires the build-time-only Python `fonttools` package. Normal builds
use the checked-in TVF1 file and only Python's standard library to embed it.
There are no downloads or font-library build steps.

The manifest records the source family/version, SHA-256, units per em, advance,
source glyph names, and exact codepoint list. Regeneration is byte-for-byte
deterministic. The original TTF is 2,573,248 bytes and is only a source asset;
it is not linked into or needed by the executable.

## Binary layout

All offsets are byte offsets; all integers are little-endian. There is no
compression, pointer serialization, implicit padding, or runtime relocation.

| Header offset | Type | Meaning |
| --- | --- | --- |
| 0 | 4 bytes | `TVF1` signature/version |
| 4 | u16 | Units per em (1000) |
| 6 | u16 | Monospace advance (600) |
| 8, 10, 12 | three i16 | Ascender, descender, line gap (1020, -300, 0) |
| 14 | u16 | Reserved, zero |
| 16 | u16 | Glyph count |
| 18 | u16 | Glyph-record size, 20 |
| 20 | u32 | Record-table offset, 32 |
| 24 | u32 | Command-stream offset |
| 28 | u32 | Total byte length |

Records are sorted by Unicode codepoint. Each 20-byte record contains a u32
codepoint, u32 command offset relative to the command stream, u16 command count,
u16 reserved zero, and four i16 control bounds: xmin, ymin, xmax, ymax.

Commands have a one-byte opcode followed by signed i16 coordinates:

| Opcode | Coordinates | Meaning |
| --- | --- | --- |
| 1 | x, y | Start contour |
| 2 | x, y | Line endpoint |
| 3 | cx, cy, x, y | Quadratic control point and endpoint |
| 4 | none | Close contour |

Font opening validates table sizes, sorted mappings, all commands/coordinates,
contour closure, and bounds. The embedded reader has no TrueType/CFF parser,
bytecode interpreter, component resolver, or shaping engine.

## Runtime rasterization and bounds

The renderer accepts physical em sizes in 26.6 pixel units, from 1 through 128
pixels. For example, 18 logical pixels at 125% becomes **22.5 physical pixels**.
It invalidates the old size's cache and rasterizes new outlines; it never
resizes a previously rasterized bitmap. Monospace advance rounds to the nearest
physical pixel for every cell; baseline and line height come from the vertical
metrics. This preserves the editor's integer-cell layout and `paint` API.

Quadratics are subdivided until midpoint deviation is at most 1/8 physical
pixel, with depth capped at 10. Nonhorizontal edges are intersected against
8 subpixel scanlines per pixel row. Sorted crossings use nonzero winding and
half-open edge intervals, then 8 horizontal samples per pixel produce coverage.
The bitmap stores 8-bit alpha with 65 possible coverage levels. There is no
hinting or LCD/subpixel rendering; 12–14 px text may be softer than FreeType's
hinted output. All glyphs share the same pixel-grid origin and baseline.

Allocations are bounded and independent of document length:

- A 48 KiB alpha arena and 64 metadata entries; a full arena or table flushes
  the cache, without growing it. Only the current physical size is cached.
- A temporary workspace of 2048 edges and 2048 crossings, about 48 KiB, allocated
  as dedicated pages on the first cache miss and released after the frame
  (`mmap`/`munmap` on Linux, `VirtualAlloc`/`VirtualFree` on Windows).
- At most 256×256 bitmap dimensions, additionally limited by the alpha arena.
  Every bundled glyph is tested through the maximum supported pixel size.
- No per-glyph malloc, unbounded recursion, persistent raster scratch space,
  or system-font discovery.

Cached bitmap pointers last until a cache flush, size change, or font close;
the editor consumes them immediately. Unknown codepoints map to the bundled
U+FFFD replacement glyph. This changes display only; file bytes are preserved.

## Supported codepoints

- U+0020–U+007E: ASCII, including normal code punctuation.
- U+00A0, U+00B0, U+00B7, U+00D7.
- U+2010, U+2013, U+2014; U+2018–U+201F; U+2022, U+2026, U+2032, U+2033.
- U+2190–U+2193: arrows.
- U+2500, U+2502, U+250C, U+2510, U+2514, U+2518: selected box drawing.
- U+E0B0, U+E0B2: Powerline dividers.
- U+F00C check, U+F040 pencil, U+F071 warning, U+F07B folder, U+F0C7 save,
  U+F111 circle, U+F15B file: explicit Nerd Font UI glyphs.
- U+FFFD: replacement glyph.

Other Nerd Font PUA ranges, nonlisted Latin accents, Greek, Cyrillic, CJK,
emoji, combining marks, wide cells, bidirectional layout, ligatures, fallback
fonts, arbitrary system fonts, and variable/color fonts are unsupported.
Tabs/newlines are handled by the editor, not as font glyphs.
