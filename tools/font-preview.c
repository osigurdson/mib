/* Optional build-time comparison tool. Only the reference build uses FreeType. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "editor.h"
#ifdef FONT_REFERENCE_FREETYPE
#include <ft2build.h>
#include FT_FREETYPE_H
#else
#include "tiny-font.h"
#endif

int main(int argc, char **argv)
{
    if (argc != 2) return 1;
    enum { WIDTH = 1450, HEIGHT = 880 };
    uint32_t *pixels = malloc((size_t)WIDTH * HEIGHT * sizeof(*pixels));
    if (!pixels) return 1;
    for (int i = 0; i < WIDTH * HEIGHT; ++i) pixels[i] = 0x18202b;
    const unsigned sizes[] = {12*64,14*64,16*64,18*64,21*64,22*64+32,27*64,36*64};
    const char *texts[] = {
        "fn main() { return 0; }  0O 1Il | ()[]{} <> +-*/= != ->",
        "\u201cquotes\u201d\u2014\u2026  \uf15b file  \uf040 edit  \uf0c7 save  \uf111 modified  \uf071 !  \ue0b0\ue0b2",
    };
#ifdef FONT_REFERENCE_FREETYPE
    FT_Library library; FT_Face face;
    if (FT_Init_FreeType(&library) || FT_New_Face(library,
        "fonts/JetBrainsMonoNerdFontMono-Regular.ttf", 0, &face)) return 1;
#else
    struct tiny_font *font = tiny_font_open(); if (!font) return 1;
#endif
    int top = 18;
    for (unsigned s = 0; s < sizeof(sizes)/sizeof(sizes[0]); ++s) {
        unsigned size = sizes[s];
#ifdef FONT_REFERENCE_FREETYPE
        if (FT_Set_Char_Size(face,0,size,72,72)) return 1;
        int ascender = (int)(face->size->metrics.ascender / 64);
        int line_height = (int)(face->size->metrics.height / 64) + 2;
        if (FT_Load_Char(face, 'M', FT_LOAD_DEFAULT | FT_LOAD_TARGET_LIGHT)) return 1;
        int advance = (int)(face->glyph->advance.x / 64);
#else
        if (!tiny_font_set_size(font, size)) return 1;
        struct tiny_font_metrics metrics = tiny_font_metrics(font);
        int ascender = metrics.ascender, line_height = metrics.line_height + 2, advance = metrics.advance;
#endif
        for (unsigned line = 0; line < 2; ++line) {
            size_t offset = 0, length = strlen(texts[line]);
            int x = 12, baseline = top + ascender + (int)line * line_height;
            while (offset < length) {
                uint32_t cp = editor_codepoint(texts[line],length,&offset);
#ifdef FONT_REFERENCE_FREETYPE
                if (FT_Load_Char(face, cp, FT_LOAD_RENDER | FT_LOAD_NO_BITMAP | FT_LOAD_TARGET_LIGHT)) return 1;
                int width = (int)face->glyph->bitmap.width, height = (int)face->glyph->bitmap.rows;
                int left = face->glyph->bitmap_left, glyph_top = face->glyph->bitmap_top;
                int pitch = face->glyph->bitmap.pitch;
                const unsigned char *alpha = face->glyph->bitmap.buffer;
#else
                const struct tiny_glyph *glyph = tiny_font_glyph(font, cp); if (!glyph) return 1;
                int width = glyph->width, height = glyph->height, left = glyph->left, glyph_top = glyph->top;
                int pitch = width;
                const unsigned char *alpha = glyph->alpha;
#endif
                for (int row = 0; row < height; ++row) {
                    for (int col = 0; col < width; ++col) {
                        int px = x + left + col, py = baseline - glyph_top + row;
                        if (px < 0 || px >= WIDTH || py < 0 || py >= HEIGHT) continue;
                        unsigned a = alpha[row * pitch + col];
                        uint32_t color = 0;
                        for (unsigned shift = 0; shift < 24; shift += 8) {
                            unsigned fg = (0xe5edf5 >> shift) & 255, bg = (0x18202b >> shift) & 255;
                            color |= ((fg * a + bg * (255 - a) + 127) / 255) << shift;
                        }
                        pixels[py * WIDTH + px] = color;
                    }
                }
                x += advance;
            }
        }
        /* Fixed per-size bands make side-by-side baseline alignment explicit. */
        top += s < 4 ? 80 : s < 6 ? 110 : 140;
    }
    FILE *out = fopen(argv[1], "wb"); if (!out) return 1;
    fprintf(out,"P6\n%d %d\n255\n",WIDTH,HEIGHT);
    for (int i = 0; i < WIDTH * HEIGHT; ++i) {
        unsigned char rgb[] = {pixels[i] >> 16,pixels[i] >> 8,pixels[i]};
        fwrite(rgb,1,3,out);
    }
    fclose(out); free(pixels);
#ifdef FONT_REFERENCE_FREETYPE
    FT_Done_Face(face); FT_Done_FreeType(library);
#else
    tiny_font_close(font);
#endif
    return 0;
}
