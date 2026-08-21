/* The navigator: the :map page selector. It draws the nine pages and maps clicks back to
 * them; everything else about it lives in this file, so it can be replaced on its own. */
#include "app.h"
#include "render-internal.h"
#include <string.h>

/* Bytes in the first columns codepoints of text. */
static size_t label_prefix(const char *text, size_t length, int columns)
{
    size_t offset = 0;
    for (int i = 0; i < columns && offset < length; ++i)
        editor_codepoint(text, length, &offset);
    return offset;
}
/* Split a page heading into at most lines rows of columns cells, breaking at a space where
 * possible and ending a cut heading with an ellipsis. */
static unsigned page_label(const char *heading, int columns, unsigned lines, char out[2][256])
{
    size_t length = strlen(heading);
    unsigned count = 0;
    while (length && count < lines && columns > 1) {
        size_t fit = label_prefix(heading, length, columns), next = fit;
        if (fit < length && count + 1 < lines) {
            size_t space = fit;
            while (space && heading[space] != ' ')
                --space;
            if (space)
                fit = next = space;
        } else if (fit < length) {
            fit = label_prefix(heading, length, columns - 1);
            next = length;
        }
        memcpy(out[count], heading, fit);
        out[count][fit] = 0;
        if (next == length && fit < length)
            strcat(out[count], "\xe2\x80\xa6");
        heading += next;
        length -= next;
        while (length && *heading == ' ')
            ++heading, --length;
        ++count;
    }
    return count;
}
static int label_columns(const char *text)
{
    int columns = 0;
    for (size_t offset = 0, length = strlen(text); offset < length; ++columns)
        editor_codepoint(text, length, &offset);
    return columns;
}
/* The :map overlay, drawn as a numeric keypad: nine square keys, each with its page number
 * and the page's first H1 underneath. The selected key uses the selected menu colors and the
 * page that is showing has an outline. */
bool navigator_draw(struct app *app, const struct palette *theme, uint32_t *pixels,
                    int width, int height, int stride, int top, int bottom, int padding)
{
    unsigned base = render_font_size(app), big = base * 2, small = base * 4 / 5;
    if (big > 127 * 64)
        big = 127 * 64;
    struct tiny_font_metrics digit = tiny_font_metrics_at(big), text = tiny_font_metrics_at(small);
    int gap = padding, edge = padding / 3 > 2 ? padding / 3 : 2, frame = padding * 3 / 2;
    int side = 12 * text.advance, fit_x = (width - 2 * frame - 2 * gap - 2 * padding) / 3,
        fit_y = (bottom - top - 2 * frame - 2 * gap - 2 * padding) / 3;
    if (side > fit_x)
        side = fit_x;
    if (side > fit_y)
        side = fit_y;
    if (side < digit.line_height + edge)
        return true;
    int grid = 3 * side + 2 * gap;
    int x0 = (width - grid) / 2, y0 = top + (bottom - top - grid) / 2;
    app->map_x = x0;
    app->map_y = y0;
    app->map_width = app->map_height = side;
    app->map_gap = gap;
    render_rounded_box(pixels, width, height, stride, x0 - frame, y0 - frame, grid + 2 * frame,
                       grid + 2 * frame, frame + padding / 2, theme->status);
    int face = side - edge, columns = (side - padding) / text.advance;
    unsigned lines = (unsigned)((face - digit.line_height - padding / 2) / text.line_height);
    if (lines > 2)
        lines = 2;
    for (unsigned page = 0; page < PAGE_COUNT; ++page) {
        int x = x0 + (int)(page % 3) * (side + gap), y = y0 + (2 - (int)(page / 3)) * (side + gap);
        bool selected = page == app->map_choice;
        uint32_t key = selected ? theme->menu_selected : theme->menu;
        uint32_t ink = selected ? theme->menu_selected_text : theme->menu_text;
        int radius = padding, ring = edge;
        /* The page that is showing keeps an outline while the selection moves. */
        if (page == app->current)
            render_rounded_box(pixels, width, height, stride, x - ring, y - ring, side + 2 * ring,
                               side + 2 * ring, radius + ring, theme->cursor);
        render_rounded_box(pixels, width, height, stride, x, y, side, side, radius,
                           render_blend(key, app->editor.light ? 40 : 96, 0xff000000));
        render_rounded_box(pixels, width, height, stride, x, y, side, face, radius, key);
        char heading[256], label_lines[2][256], number[2] = {(char)('1' + page), 0};
        app_page_heading(app, page, heading, sizeof(heading));
        unsigned count = page_label(heading, columns, lines, label_lines);
        int block = digit.line_height + (int)count * text.line_height;
        int line_y = y + (face - block) / 2;
        if (!tiny_font_set_size(app->font, big) ||
            !render_label(app, pixels, width, height, stride, number, x + (side - digit.advance) / 2,
                          line_y + digit.ascender, digit.advance, ink))
            return false;
        line_y += digit.line_height;
        if (count && !tiny_font_set_size(app->font, small))
            return false;
        for (unsigned i = 0; i < count; ++i, line_y += text.line_height) {
            int span = label_columns(label_lines[i]) * text.advance;
            if (!render_label(app, pixels, width, height, stride, label_lines[i], x + (side - span) / 2,
                              line_y + text.ascender, text.advance, ink))
                return false;
        }
    }
    return tiny_font_set_size(app->font, base);
}
/* The page whose key is under physical pixel (x, y) in the last paint, or -1. */
int navigator_page_at(const struct app *app, int x, int y)
{
    int step_x = app->map_width + app->map_gap, step_y = app->map_height + app->map_gap;
    if (step_x <= 0 || step_y <= 0 || x < app->map_x || y < app->map_y)
        return -1;
    int column = (x - app->map_x) / step_x, row = (y - app->map_y) / step_y;
    if (column > 2 || row > 2 || (x - app->map_x) % step_x >= app->map_width ||
        (y - app->map_y) % step_y >= app->map_height)
        return -1;
    return (2 - row) * 3 + column;
}
