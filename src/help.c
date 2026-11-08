/* The :help card: the few things a Vim user needs that Vim doesn't teach them. Esc, q, Enter or
 * a click closes it (see app.c). */
#include "app.h"
#include "render-internal.h"

enum { HELP_TITLE, HELP_SECTION, HELP_ROW, HELP_FOOTER };
static const struct {
    unsigned char kind;
    const char *keys, *action;
} lines[] = {
    {HELP_TITLE, "mib: Vim keys for Markdown notes", ""},
    {HELP_SECTION, "Views", ""},
    {HELP_ROW, ":set view=normal", "styled Markdown (default)"},
    {HELP_ROW, ":set view=source", "plain source"},
    {HELP_ROW, ":set view=reading", "read-only reading view"},
    {HELP_SECTION, "Pages", ""},
    {HELP_ROW, "Alt+1 \xe2\x80\xa6 Alt+9", "show page 1 to 9"},
    {HELP_ROW, "Alt+0, :ls", "list the pages"},
    {HELP_ROW, ":b N", "show page N"},
    {HELP_FOOTER, "Esc closes", ""},
};
enum { HELP_LINES = sizeof(lines) / sizeof(lines[0]), HELP_KEYS = 20, HELP_COLUMNS = 48 };

bool help_draw(struct app *app, const struct palette *theme, uint32_t *pixels, int width,
               int height, int stride, int top, int bottom, int padding)
{
    if (!tiny_font_set_size(app->font, render_font_size(app)))
        return false;
    struct tiny_font_metrics metrics = tiny_font_metrics(app->font);
    int cell = metrics.advance, row = metrics.line_height, frame = 2 * padding;
    /* A blank row before each section and the footer. */
    int rows = HELP_LINES;
    for (unsigned i = 1; i < HELP_LINES; ++i)
        rows += lines[i].kind != HELP_ROW;
    int box_w = HELP_COLUMNS * cell + 2 * frame, box_h = rows * row + 2 * frame;
    if (box_w > width - 2 * padding || box_h > bottom - top - 2 * padding)
        return true; /* too small to read; the next key still closes it */
    int x0 = (width - box_w) / 2, y0 = top + (bottom - top - box_h) / 2;
    render_rounded_box(pixels, width, height, stride, x0, y0, box_w, box_h, padding, theme->menu);
    int x = x0 + frame, y = y0 + frame;
    for (unsigned i = 0; i < HELP_LINES; ++i) {
        if (i && lines[i].kind != HELP_ROW)
            y += row;
        int baseline = y + metrics.ascender, indent = lines[i].kind == HELP_ROW ? 2 * cell : 0;
        uint32_t color = lines[i].kind == HELP_TITLE ? theme->heading
                         : lines[i].kind == HELP_ROW ? theme->menu_text
                                                     : theme->dim;
        if (!render_label(app, pixels, width, height, stride, lines[i].keys, x + indent, baseline,
                          cell, color) ||
            (lines[i].action[0] &&
             !render_label(app, pixels, width, height, stride, lines[i].action,
                           x + indent + HELP_KEYS * cell, baseline, cell, theme->dim)))
            return false;
        y += row;
    }
    return true;
}
