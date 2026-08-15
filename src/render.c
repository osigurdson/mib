#include "app.h"
#include <stdio.h>
#include <string.h>

static uint32_t blend(uint32_t background, unsigned alpha)
{
    const uint32_t foreground = 0xffe5edf5;
    uint32_t pixel = 0xff000000;
    for (unsigned shift = 0; shift < 24; shift += 8) {
        unsigned fg = (foreground >> shift) & 255;
        unsigned bg = (background >> shift) & 255;
        pixel |= ((fg * alpha + bg * (255 - alpha) + 127) / 255) << shift;
    }
    return pixel;
}

static void rectangle(uint32_t *pixels, int width, int height, int stride,
                      int x, int y, int w, int h, uint32_t color)
{
    for (int row = y < 0 ? 0 : y; row < height && row < y + h; ++row)
        for (int col = x < 0 ? 0 : x; col < width && col < x + w; ++col)
            pixels[(size_t)row * (size_t)stride + (size_t)col] = color;
}

static bool glyph(struct app *app, uint32_t *pixels, int width, int height, int stride,
                  uint32_t cp, int x, int baseline, int clip_bottom)
{
    const struct tiny_glyph *bitmap = tiny_font_glyph(app->font, cp);
    if (!bitmap) return false;
    for (int row = 0; row < bitmap->height; ++row) {
        int y = baseline - bitmap->top + row;
        if (y < 0 || y >= height || y >= clip_bottom) continue;
        const unsigned char *coverage = bitmap->alpha + (size_t)row * bitmap->width;
        for (int col = 0; col < bitmap->width; ++col) {
            int px = x + bitmap->left + col;
            if (px < 0 || px >= width) continue;
            size_t offset = (size_t)y * (size_t)stride + (size_t)px;
            pixels[offset] = blend(pixels[offset], coverage[col]);
        }
    }
    return true;
}

static bool label(struct app *app, uint32_t *pixels, int width, int height, int stride,
                  const char *text, int x, int baseline, int cell)
{
    size_t length = strlen(text), offset = 0;
    while (offset < length && x < width - cell) {
        uint32_t cp = editor_codepoint(text, length, &offset);
        if (cp < 32 || cp == 127) cp = '?';
        if (!glyph(app, pixels, width, height, stride, cp, x, baseline, height)) return false;
        x += cell;
    }
    return true;
}

static bool paint(struct app *app, uint32_t *pixels, int width, int height, int stride)
{
    struct editor *ed = &app->editor;
    unsigned size = (unsigned)((18 * 64 * (int64_t)app->scale120 + 60) / 120);
    if (!tiny_font_set_size(app->font, size)) return false;
    struct tiny_font_metrics metrics = tiny_font_metrics(app->font);
    int cell = metrics.advance;
    int ascender = metrics.ascender;
    int line_height = metrics.line_height + 2;
    int padding = (int)((8 * (int64_t)app->scale120 + 60) / 120);
    int status_y = height - 2 * line_height - padding;
    int rows = (status_y - padding) / line_height;
    int columns = (width - 2 * padding) / cell;
    if (rows < 1) rows = 1;
    if (columns < 1) columns = 1;
    size_t cursor_row = 0;
    for (size_t i = 0; i < ed->cursor; ++i) if (ed->text[i] == '\n') ++cursor_row;
    size_t cursor_column = editor_column(ed, ed->cursor);
    if (cursor_row < ed->top) ed->top = cursor_row;
    if (cursor_row - ed->top >= (size_t)rows) ed->top = cursor_row - (size_t)rows + 1;
    if (cursor_column < ed->left) ed->left = cursor_column;
    if (cursor_column - ed->left >= (size_t)columns) ed->left = cursor_column - (size_t)columns + 1;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            pixels[(size_t)y * (size_t)stride + (size_t)x] = 0xff18202b;

    size_t start = 0;
    for (size_t row = 0; row < ed->top && start < ed->length; ++row) {
        start = editor_line_end(ed, start);
        if (start < ed->length) ++start;
    }
    for (int row = 0; row < rows; ++row) {
        size_t end = editor_line_end(ed, start);
        size_t offset = start, column = 0;
        while (offset < end) {
            uint32_t cp = editor_codepoint(ed->text, end, &offset);
            size_t advance = cp == '\t' ? 4 - column % 4 : 1;
            if (column >= ed->left && column - ed->left >= (size_t)columns) break;
            if (column >= ed->left && cp != '\t') {
                int x = padding + (int)(column - ed->left) * cell;
                int y = padding + row * line_height;
                if (cp < 32 || cp == 127) cp = '?';
                if (!glyph(app, pixels, width, height, stride, cp, x, y + ascender, status_y)) return false;
            }
            column += advance;
        }
        if (end == ed->length) break;
        start = end + 1;
    }
    int cursor_x = padding + (int)(cursor_column - ed->left) * cell;
    int cursor_y = padding + (int)(cursor_row - ed->top) * line_height;
    if (!ed->command_active) {
        if (ed->mode == INSERT)
            rectangle(pixels, width, height, stride, cursor_x, cursor_y, 2, line_height, 0xff8fbcbb);
        else
            rectangle(pixels, width, height, stride, cursor_x, cursor_y + line_height - 3,
                      cell, 3, 0xff8fbcbb);
    }
    rectangle(pixels, width, height, stride, 0, status_y, width, line_height + 2 * padding, 0xff283443);
    char status[512];
    snprintf(status, sizeof(status), "%s  \uf15b %s%s  |  %zu:%zu",
             ed->mode == INSERT ? "\uf040 INSERT" : "NORMAL",
             ed->filename ? ed->filename : "[No Name]", ed->modified ? " [+] \uf111" : "",
             cursor_row + 1, cursor_column + 1);
    if (!label(app, pixels, width, height, stride, status, padding, status_y + ascender, cell)) return false;
    char prompt[1100];
    if (ed->command_active) snprintf(prompt, sizeof(prompt), ":%s_", ed->command);
    else snprintf(prompt, sizeof(prompt), "%s", ed->message[0] ? ed->message :
                  "i insert  Esc normal  hjkl move  :w :q :wq");
    return label(app, pixels, width, height, stride, prompt, padding,
                 status_y + line_height + ascender, cell);
}

bool app_paint(struct app *app, uint32_t *pixels, int width, int height, int stride)
{
    if (!pixels || width < 1 || height < 1 || stride < width) return false;
    if (!app->font) app->font = tiny_font_open();
    if (!app->font) return false;
    bool ok = paint(app, pixels, width, height, stride);
    tiny_font_end_frame(app->font);
    return ok;
}
