# Custom font pipeline results

Measured on the same Wayland desktop at 150% scale, with an 800×560 logical
window, a real empty file, and one editor process at a time. Raw observations
are in [font-memory-measurements.json](font-memory-measurements.json). Five
interleaved before/after runs were sampled 0.8 seconds after launch. Values
are idle process memory, not rendering peaks or compositor storage.

| Measure | Previous private FreeType renderer | Custom TVF1 renderer |
| --- | ---: | ---: |
| Default executable, including debug information | 118,088 bytes | 149,896 bytes |
| Stripped executable | 35,872 bytes | 60,384 bytes |
| Additional runtime font library | 717,472-byte private FreeType `.so` | None |
| Embedded font subset | None | 19,164 bytes, 134 glyphs |
| Idle RSS, median | 2,824 KiB | 2,828 KiB |
| Idle RSS, range | 2,760–2,832 KiB | 2,764–2,828 KiB |
| Idle PSS, median | 863 KiB | 867 KiB |
| Private dirty pages, median | 552 KiB | 552 KiB |

Both executables are comfortably below 1 MiB. The new executable is larger
because it includes its font and rasterizer, but no additional runtime font
library or font file is needed. The full bundled TTF is only for regeneration.

**Idle RSS is essentially unchanged**, within normal measurement variation.
The previous renderer already closed its font and unloaded its private FreeType
library after each frame. It had no idle FreeType, Fontconfig, or HarfBuzz
mappings to remove. The custom renderer trades some of the old allocator/parser
residue for a read-only outline subset and a small persistent glyph cache.
Glyph outlines stay embedded in the executable, and cached glyph bitmaps remain
in memory. There is no per-redraw font-file open, library reload, or filesystem
write. Changing scale invalidates bitmaps and rasterizes the embedded outlines
again. Releasing the temporary workspace unmaps anonymous memory, not files.

This work does not achieve sub-1-MiB RSS; the remaining Wayland/xkbcommon/libc
runtime still dominates the client.

## Font-library accounting

- Before: `readelf -d tiny-vi` lists Wayland, xkbcommon, and libc. FreeType was
  loaded with `dlopen` only while rendering, from a separate 717,472-byte private
  build. Its code was not embedded in the executable, and it contributed zero
  idle mapped pages in the pre-change samples. The client contained its own
  loader calls and rendering adapter.
- Fontconfig and HarfBuzz already contributed zero linked code and zero idle
  mappings: discovery happened at build time, and the private FreeType build
  disabled HarfBuzz. Removing them therefore cannot be credited as an RSS win.
- After: the executable links the same three runtime libraries. It has no
  `dlopen`, FreeType, HarfBuzz, or Fontconfig calls and never loads them, even
  during a frame. The custom rasterizer, TVF1 reader, and 19,164-byte subset
  are part of the executable. No cached FreeType build is needed to run it.

The earlier renderer used Nimbus Mono PS. The visual reference below instead
uses **the exact same bundled JetBrains Mono Nerd Font Mono** as the custom
renderer, so differences in the images isolate rasterization rather than font
family. The reference is a separate optional build-time tool, not an editor
runtime dependency.

## Visual review

[Paired FreeType/custom samples at native pixel sizes](font-comparison.png)
compare 12, 14, 16, 18, 21, 22.5, 27, and 36 physical-pixel em sizes. They include
code punctuation, ambiguous digits/letters, curly quotes, em dash, ellipsis,
file/edit/save/modified/warning Nerd Font glyphs, and Powerline dividers.
The 21, 22.5, and 27 px rows exercise fractional-scale targets; 36 px represents
18 logical pixels at 2×. Neither image is bitmap-resized.

The custom output is reasonably crisp and preserves holes and punctuation
throughout this range. Small text is slightly softer than the hinted FreeType
reference; there is no font bytecode or stem hinting in this implementation.
Icons remain legible, with grayscale edges and intact interior holes. Text is
unshaped: `!=`, `->`, and other sequences stay separate monospace glyphs.

Regenerate the comparison with:

```sh
python3 tools/compare-fonts.py
```

This optional QA command requires Pillow and system FreeType development files.
Normal building, tests, and running the editor do not require FreeType.

## Checks

- Editor movement, keyboard, save/quit, scrolling, and resize/clipping tests.
- Every subset glyph at physical sizes 1, 12, 14, 16, 18, 21, 22.5, 27, 36,
  and 128 px, including cache eviction and size invalidation.
- TVF1 truncation/malformed-header/command rejection, winding holes, antialiasing,
  and fixed cache/workspace bounds.
- AddressSanitizer/UndefinedBehaviorSanitizer runs of the font and client tests.
- Byte-identical subset regeneration from the bundled font.
- Native Wayland smoke test: 150% preferred scale, 1200×840 shared-memory frame,
  and compositor buffer release without protocol errors.
