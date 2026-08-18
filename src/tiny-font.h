#ifndef TINY_FONT_H
#define TINY_FONT_H
#include <stdbool.h>
#include <stdint.h>

struct tiny_font;
struct tiny_font_metrics {
    int advance, ascender, descender, line_height;
};
struct tiny_glyph {
    int width, height, left, top;
    const uint8_t *alpha;
};

struct tiny_font *tiny_font_open(void);
void tiny_font_close(struct tiny_font *font);
/* Physical em size in 26.6 pixels; supported range 1..128 pixels. */
bool tiny_font_set_size(struct tiny_font *font, unsigned pixels64);
struct tiny_font_metrics tiny_font_metrics(const struct tiny_font *font);
/* Measure another size without changing the active raster size or cache. */
struct tiny_font_metrics tiny_font_metrics_at(unsigned pixels64);
/* Bitmap remains valid until a cache flush or close. */
const struct tiny_glyph *tiny_font_glyph(struct tiny_font *font, uint32_t codepoint);
/* Release temporary flattening/scanline workspace, retaining cached glyphs. */
void tiny_font_end_frame(struct tiny_font *font);
#endif
