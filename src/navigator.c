/* The navigator: the :map page selector. It draws the nine pages and maps clicks back to
 * them; everything else about it lives in this file, so it can be replaced on its own. */
#include "app.h"
#include "render-internal.h"
#include <stdio.h>
#include <string.h>

enum { NAVIGATOR_COLUMNS = 48 };

/* The first columns cells of text, ending with an ellipsis if it is cut. */
static void fit_text(const char *text, int columns, char *out, size_t size)
{
    size_t length = strlen(text), offset = 0;
    int count = 0;
    while (offset < length && count < columns) {
        editor_codepoint(text, length, &offset);
        ++count;
    }
    if (offset < length && columns > 1) { /* make room for the ellipsis */
        offset = 0;
        for (int i = 0; i < columns - 1; ++i)
            editor_codepoint(text, length, &offset);
        snprintf(out, size, "%.*s\xe2\x80\xa6", (int)offset, text);
    } else {
        snprintf(out, size, "%.*s", (int)offset, text);
    }
}
/* The :map overlay: a menu listing the nine pages top to bottom, each row its number and
 * the page's first H1 (or a dash). The selected row uses the selected menu colors and the
 * page that is showing has a dot at the end of its row. */
bool navigator_draw(struct app *app, const struct palette *theme, uint32_t *pixels,
                    int width, int height, int stride, int top, int bottom, int padding)
{
    unsigned base = render_font_size(app);
    if (!tiny_font_set_size(app->font, base))
        return false;
    struct tiny_font_metrics metrics = tiny_font_metrics(app->font);
    int cell = metrics.advance, row = metrics.line_height + padding, frame = padding;
    int list_w = (NAVIGATOR_COLUMNS + 6) * cell, list_h = PAGE_COUNT * row;
    if (list_w > width - 4 * frame)
        list_w = width - 4 * frame;
    if (list_w < 12 * cell || list_h > bottom - top - 2 * frame)
        return true;
    int x0 = (width - list_w) / 2, y0 = top + (bottom - top - list_h) / 2;
    app->map_x = x0;
    app->map_y = y0;
    app->map_width = list_w;
    app->map_height = row;
    render_rounded_box(pixels, width, height, stride, x0 - frame, y0 - frame, list_w + 2 * frame,
                       list_h + 2 * frame, frame, theme->menu);
    int title_columns = list_w / cell - 6, dot = padding / 2 > 2 ? padding / 2 : 2;
    for (unsigned page = 0; page < PAGE_COUNT; ++page) {
        int y = y0 + (int)page * row, baseline = y + padding / 2 + metrics.ascender;
        bool selected = page == app->map_choice;
        uint32_t ink = selected ? theme->menu_selected_text : theme->menu_text;
        uint32_t dim = selected ? theme->menu_selected_text : theme->dim;
        if (selected)
            render_rounded_box(pixels, width, height, stride, x0, y, list_w, row, padding / 2,
                               theme->menu_selected);
        char heading[256], title[256], number[2] = {(char)('1' + page), 0};
        app_page_heading(app, page, heading, sizeof(heading));
        fit_text(heading[0] ? heading : "\xe2\x80\x94", title_columns, title, sizeof(title));
        if (!render_label(app, pixels, width, height, stride, number, x0 + cell, baseline, cell,
                          dim) ||
            !render_label(app, pixels, width, height, stride, title, x0 + 3 * cell, baseline,
                          cell, heading[0] ? ink : dim))
            return false;
        if (page == app->current) /* the page that is showing */
            render_rounded_box(pixels, width, height, stride, x0 + list_w - cell - dot,
                               y + (row - 2 * dot) / 2, 2 * dot, 2 * dot, dot, ink);
    }
    return true;
}
/* The page whose row is under physical pixel (x, y) in the last paint, or -1. */
int navigator_page_at(const struct app *app, int x, int y)
{
    if (app->map_height <= 0 || x < app->map_x || x >= app->map_x + app->map_width ||
        y < app->map_y || y >= app->map_y + PAGE_COUNT * app->map_height)
        return -1;
    return (y - app->map_y) / app->map_height;
}
