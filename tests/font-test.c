#include "../tiny-font.c"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    assert(valid_font(font_data, sizeof(font_data)));
    for (size_t length = 0; length < sizeof(font_data); ++length)
        assert(!valid_font(font_data, length));
    unsigned char *broken = malloc(sizeof(font_data)); assert(broken);
    const unsigned offsets[] = {0, 16, 18, 20, 24, 28};
    for (unsigned i = 0; i < sizeof(offsets)/sizeof(offsets[0]); ++i) {
        memcpy(broken, font_data, sizeof(font_data));
        broken[offsets[i]] ^= 0xff;
        assert(!valid_font(broken, sizeof(font_data)));
    }
    memcpy(broken, font_data, sizeof(font_data));
    broken[4] = broken[5] = 0;
    assert(!valid_font(broken, sizeof(font_data)));
    memcpy(broken, font_data, sizeof(font_data));
    broken[u32(font_data + 24)] = 99;
    assert(!valid_font(broken, sizeof(font_data)));
    free(broken);

    struct tiny_font *font = tiny_font_open(); assert(font);
    assert(!tiny_font_set_size(font, 0));
    assert(!tiny_font_set_size(font, 128 * 64 + 1));
    const unsigned sizes[] = {64, 12*64, 14*64, 16*64, 18*64, 21*64, 22*64+32, 27*64, 36*64, 128*64};
    for (unsigned s = 0; s < sizeof(sizes)/sizeof(sizes[0]); ++s) {
        assert(tiny_font_set_size(font, sizes[s]));
        for (unsigned i = 0; i < u16(font_data + 16); ++i) {
            uint32_t cp = u32(font_data + HEADER_BYTES + i * RECORD_BYTES);
            const struct tiny_glyph *glyph = tiny_font_glyph(font, cp);
            if (!glyph) fprintf(stderr,"Failed U+%04X size %u\n", cp, sizes[s]);
            assert(glyph);
            assert(glyph->width <= MAX_DIMENSION && glyph->height <= MAX_DIMENSION);
            assert(font->used <= CACHE_BYTES && font->count <= CACHE_SLOTS);
            assert(tiny_font_glyph(font, cp) == glyph);
        }
        tiny_font_end_frame(font); assert(!font->workspace);
    }
    assert(tiny_font_set_size(font, 27 * 64));
    const struct tiny_glyph *replacement = tiny_font_glyph(font, 0xfffd);
    assert(tiny_font_glyph(font, 0x1f600) == replacement);
    const struct tiny_glyph *o = tiny_font_glyph(font, 'O');
    assert(o && o->width > 4 && o->height > 4);
    assert(o->alpha[(o->height/2) * o->width + o->width/2] == 0); /* A counter, not a solid polygon. */
    bool fractional = false;
    for (int i = 0; i < o->width * o->height; ++i)
        if (o->alpha[i] > 0 && o->alpha[i] < 255) fractional = true;
    assert(fractional);
    struct tiny_font_metrics metrics = tiny_font_metrics(font);
    assert(metrics.advance == 16 && metrics.ascender > 0 && metrics.descender < 0);
    tiny_font_close(font);
    puts("TVF1 validation, all subset glyphs/sizes, cache bounds, winding, and antialiasing passed");
}
