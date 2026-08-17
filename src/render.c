#include "app.h"
#include "markdown.h"
#include "media.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

struct palette {
    uint32_t background, text, dim, link, heading, quote, done, italic;
    uint32_t code, search, selection, cursor, status, menu, menu_selected, handle;
};
static const struct palette dark = {
    0xff18202b, 0xffe5edf5, 0xff697889, 0xff88c0d0,
    0xffebcb8b, 0xffa3be8c, 0xff77868a, 0xffb9a0cb,
    0xff242d3b, 0xff66502c, 0xff405c78, 0xff8fbcbb,
    0xff283443, 0xff34485b, 0xff4b6680, 0xffe5edf5,
};
static const struct palette light = {
    0xfffaf9f6, 0xff263342, 0xff697685, 0xff176778,
    0xff855a16, 0xff416b36, 0xff687567, 0xff755088,
    0xffeeede7, 0xfff5dc8e, 0xffc5dced, 0xff1d6972,
    0xffe5e8eb, 0xffe9edf1, 0xffcbdbe8, 0xff263342,
};
static const struct palette *colors(const struct app *app)
{
    return app->editor.light ? &light : &dark;
}

static uint32_t blend(uint32_t background, unsigned alpha, uint32_t foreground)
{
    uint32_t pixel = 0xff000000;
    for (unsigned shift = 0; shift < 24; shift += 8) {
        unsigned fg = (foreground >> shift) & 255;
        unsigned bg = (background >> shift) & 255;
        pixel |= ((fg * alpha + bg * (255 - alpha) + 127) / 255) << shift;
    }
    return pixel;
}

static void rectangle(uint32_t *pixels, int width, int height, int stride, int x, int y, int w,
                      int h, uint32_t color)
{
    for (int row = y < 0 ? 0 : y; row < height && row < y + h; ++row)
        for (int col = x < 0 ? 0 : x; col < width && col < x + w; ++col)
            pixels[(size_t)row * (size_t)stride + (size_t)col] = color;
}

static bool glyph(struct app *app, uint32_t *pixels, int width, int height, int stride, uint32_t cp,
                  int x, int baseline, int clip_bottom, uint32_t color)
{
    const struct tiny_glyph *bitmap = tiny_font_glyph(app->font, cp);
    if (!bitmap)
        return false;
    for (int row = 0; row < bitmap->height; ++row) {
        int y = baseline - bitmap->top + row;
        if (y < 0 || y >= height || y >= clip_bottom)
            continue;
        const unsigned char *coverage = bitmap->alpha + (size_t)row * bitmap->width;
        for (int col = 0; col < bitmap->width; ++col) {
            int px = x + bitmap->left + col;
            if (px < 0 || px >= width)
                continue;
            size_t offset = (size_t)y * (size_t)stride + (size_t)px;
            pixels[offset] = blend(pixels[offset], coverage[col], color);
        }
    }
    return true;
}

static bool label(struct app *app, uint32_t *pixels, int width, int height, int stride,
                  const char *text, int x, int baseline, int cell)
{
    const struct palette *theme = colors(app);
    size_t length = strlen(text), offset = 0;
    while (offset < length && x < width - cell) {
        uint32_t cp = editor_codepoint(text, length, &offset);
        if (cp < 32 || cp == 127)
            cp = '?';
        if (!glyph(app, pixels, width, height, stride, cp, x, baseline, height, theme->text))
            return false;
        x += cell;
    }
    return true;
}

static const char *leaf_name(const char *path)
{
    const char *slash = strrchr(path, '/'), *back = strrchr(path, '\\');
    if (back && (!slash || back > slash))
        slash = back;
    return slash ? slash + 1 : path;
}

static bool title_bar(struct app *app, uint32_t *pixels, int width, int height, int stride,
                      int ascender)
{
    const struct palette *theme = colors(app);
    int h = app->title_height;
    rectangle(pixels, width, height, stride, 0, 0, width, h, theme->status);
    rectangle(pixels, width, height, stride, 0, h - 1, width, 1, theme->dim);
    const char *name = app->scratch && app->directory ? leaf_name(app->directory)
                       : app->editor.filename ? leaf_name(app->editor.filename) : "";
    char title[512];
    snprintf(title, sizeof(title), "Jot%s%s%s", *name ? " — " : "", name,
             app->editor.modified ? " [+]" : "");
    if (!label(app, pixels, width - 3 * h, height, stride, title, app->padding,
               app->padding + ascender, app->cell))
        return false;
    int size = h / 4, stroke = (int)(app->scale120 / 120);
    if (size < 3)
        size = 3;
    if (stroke < 1)
        stroke = 1;
    for (int button = 0; button < 3; ++button) {
        int x = width - (3 - button) * h + (h - size) / 2, y = (h - size) / 2;
        if (button == 0)
            rectangle(pixels, width, height, stride, x, y + size, size, stroke, theme->text);
        else if (button == 1) {
            int offset = app->maximized ? 3 * stroke : 0;
            rectangle(pixels, width, height, stride, x + offset, y - offset, size, stroke, theme->text);
            rectangle(pixels, width, height, stride, x + size + offset - stroke, y - offset,
                      stroke, size, theme->text);
            rectangle(pixels, width, height, stride, x, y, size, stroke, theme->text);
            rectangle(pixels, width, height, stride, x, y, stroke, size, theme->text);
            rectangle(pixels, width, height, stride, x + size - stroke, y, stroke, size, theme->text);
            rectangle(pixels, width, height, stride, x, y + size - stroke, size, stroke, theme->text);
        } else
            for (int i = 0; i < size; ++i) {
                rectangle(pixels, width, height, stride, x + i, y + i, stroke, stroke, theme->text);
                rectangle(pixels, width, height, stride, x + size - i - 1, y + i,
                          stroke, stroke, theme->text);
            }
    }
    return true;
}

enum app_title_action app_title_hit(const struct app *app, int logical_x, int logical_y)
{
    int x = (int)((int64_t)logical_x * app->scale120 / 120),
        y = (int)((int64_t)logical_y * app->scale120 / 120), h = app->title_height;
    if (!app->client_decorated || h <= 0 || y < 0 || y >= h || x < 0 || x >= app->view_width)
        return TITLE_NONE;
    if (x >= app->view_width - h)
        return TITLE_CLOSE;
    if (x >= app->view_width - 2 * h)
        return TITLE_MAXIMIZE;
    if (x >= app->view_width - 3 * h)
        return TITLE_MINIMIZE;
    return TITLE_MOVE;
}

/* A view row ends at a word boundary where possible, otherwise at a character. */
size_t app_row_next(const struct editor *ed, size_t start, unsigned columns)
{
    size_t p = start, last_space = SIZE_MAX;
    unsigned col = 0;
    while (p < ed->length && ed->text[p] != '\n') {
        unsigned step = ed->text[p] == '\t' ? 4 - col % 4 : 1;
        if (col + step > columns) {
            if (last_space != SIZE_MAX && last_space > start)
                return last_space;
            return p > start ? p : editor_next(ed, p);
        }
        col += step;
        p = editor_next(ed, p);
        if (ed->text[p - 1] == ' ' || ed->text[p - 1] == '\t')
            last_space = p;
    }
    return p < ed->length ? p + 1 : p;
}
size_t app_view_next(struct app *app, size_t start)
{
    const struct media_entry *image = media_at(app, start);
    if (image && image->width > 0)
        return image->end < app->editor.length ? image->end + 1 : image->end;
    return app_row_next(&app->editor, start,
                        app->editor.viewport_columns ? app->editor.viewport_columns : 80);
}
int app_view_rows(struct app *app, size_t start)
{
    return media_rows(app, start, app->view_width - 2 * app->padding - app->gutter,
                      app->line_height ? app->line_height : 24,
                      app->editor.viewport_rows ? app->editor.viewport_rows : 24);
}
static void draw_image(struct app *app, const struct media_entry *entry, uint32_t *pixels,
                       int width, int height, int stride, int x, int y, int available_width,
                       int available_height, bool selected)
{
    const struct palette *theme = colors(app);
    struct image image = {0};
    if (available_width < 1 || available_height < 1 || !media_pixels(app, entry, &image))
        return;
    int w = image.width, h = image.height;
    if (w > available_width) {
        h = (int)((int64_t)h * available_width / w);
        w = available_width;
    }
    if (h > available_height) {
        w = (int)((int64_t)w * available_height / h);
        h = available_height;
    }
    if (w < 1)
        w = 1;
    if (h < 1)
        h = 1;
    if (selected)
        rectangle(pixels, width, height, stride, x - 3, y - 3, w + 6, h + 6, theme->link);
    for (int row = 0; row < h && row + y < height; ++row)
        for (int col = 0; col < w && col + x < width; ++col) {
            if (row + y < 0 || col + x < 0)
                continue;
            const unsigned char *p =
                image.rgba + ((size_t)((int64_t)row * image.height / h) * image.width +
                              (size_t)((int64_t)col * image.width / w)) *
                                 4;
            uint32_t color = 0xff000000 | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
            size_t at = (size_t)(row + y) * stride + col + x;
            pixels[at] = blend(pixels[at], p[3], color);
        }
    media_highlight(app, entry, pixels, width, height, stride, x, y, w, h);
    if (selected) {
        /* Corner handles keep selection visible against light or busy images. */
        for (int corner = 0; corner < 4; ++corner)
            rectangle(pixels, width, height, stride,
                      (corner & 1) ? x + w - 3 : x - 3,
                      (corner & 2) ? y + h - 3 : y - 3, 6, 6, theme->handle);
    }
    image_free(&image);
}
static bool highlight(const struct editor *ed, size_t offset)
{
    if (!ed->search[0])
        return false;
    size_t n = strlen(ed->search), begin = offset >= n - 1 ? offset - n + 1 : 0;
    for (size_t p = begin; p <= offset && p + n <= ed->length; ++p)
        if (editor_match_at(ed, p))
            return true;
    return false;
}
static bool paint(struct app *app, uint32_t *pixels, int width, int height, int stride)
{
    struct editor *ed = &app->editor;
    const struct palette *theme = colors(app);
    unsigned size =
        (unsigned)(((app->font_size ? app->font_size : 18) * 64 * (int64_t)app->scale120 + 60) /
                   120);
    if (!tiny_font_set_size(app->font, size))
        return false;
    struct tiny_font_metrics metrics = tiny_font_metrics(app->font);
    int cell = metrics.advance, ascender = metrics.ascender, line_height = metrics.line_height + 2;
    int padding = (int)((8 * (int64_t)app->scale120 + 60) / 120);
    app->title_height = app->client_decorated ? line_height + 2 * padding : 0;
    int top = app->title_height + padding;
    int status_y = height - line_height - 2 * padding, rows = (status_y - top) / line_height;
    size_t lines = editor_line_count(ed);
    int digits = 1;
    for (size_t n = lines; n >= 10; n /= 10)
        ++digits;
    if (digits < 5)
        digits = 5;
    int gutter = (digits + 1) * cell;
    int columns = (width - 2 * padding - gutter) / cell;
    if (rows < 1)
        rows = 1;
    if (columns < 1)
        columns = 1;
    ed->viewport_rows = (unsigned)rows;
    ed->viewport_columns = (unsigned)columns;
    app->view_width = width;
    media_sync(app);
    app->cell = cell;
    app->line_height = line_height;
    app->padding = padding;
    app->gutter = gutter;
    if (ed->top > ed->length)
        ed->top = editor_line_start(ed, ed->length);
    if (app->follow_cursor) {
        if (ed->cursor < ed->top)
            ed->top = editor_line_start(ed, ed->cursor);
        size_t p = ed->top, cursor_row = 0;
        while (p < ed->cursor) {
            size_t next = app_view_next(app, p);
            if (next <= p || next > ed->cursor ||
                (next == ed->cursor && ed->text[next - 1] != '\n' && next == ed->length))
                break;
            cursor_row += (size_t)app_view_rows(app, p);
            p = next;
        }
        size_t remove = cursor_row >= (size_t)rows ? cursor_row - (size_t)rows + 1 : 0;
        while (remove) {
            size_t step = (size_t)app_view_rows(app, ed->top), next = app_view_next(app, ed->top);
            if (next == ed->top)
                break;
            ed->top = next;
            remove = remove > step ? remove - step : 0;
        }
    }
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            pixels[(size_t)y * stride + x] = theme->background;
    if (app->client_decorated && !title_bar(app, pixels, width, height, stride, ascender))
        return false;
    size_t selection_a = 0, selection_b = 0;
    bool linewise;
    bool selected = editor_selection(ed, &selection_a, &selection_b, &linewise);
    char fenced = 0;
    for (size_t p = 0; p < editor_line_start(ed, ed->top);) {
        char marker = editor_fence(ed, p);
        if (marker)
            fenced = fenced ? (fenced == marker ? 0 : fenced) : marker;
        size_t end = editor_line_end(ed, p);
        if (end == ed->length)
            break;
        p = end + 1;
    }
    size_t start = ed->top, last_line = SIZE_MAX;
    int cursor_x = -1, cursor_y = -1;
    for (int row = 0; row < rows; ++row) {
        size_t next = app_view_next(app, start), end = next;
        if (end > start && ed->text[end - 1] == '\n')
            --end;
        size_t logical_start = editor_line_start(ed, start),
               logical_end = editor_line_end(ed, start);
        char is_fence = editor_fence(ed, logical_start);
        if (logical_start != last_line && is_fence)
            fenced = fenced ? (fenced == is_fence ? 0 : fenced) : is_fence;
        last_line = logical_start;
        struct md_span spans[128];
        size_t span_count = ed->markdown ? markdown_line(ed->text, logical_start, logical_end,
                                                         spans, 128, fenced || is_fence)
                                         : 0;
        int y = top + row * line_height;
        if (start == logical_start) {
            char number[32];
            snprintf(number, sizeof(number), "%*zu", digits, editor_line_number(ed, start) + 1);
            uint32_t color = logical_start == editor_line_start(ed, ed->cursor)
                                 ? theme->text : theme->dim;
            for (int i = 0; number[i]; ++i)
                if (!glyph(app, pixels, width, height, stride, (unsigned char)number[i],
                           padding + i * cell, y + ascender, status_y, color))
                    return false;
        }
        const struct media_entry *image = media_at(app, start);
        if (image && image->width > 0) {
            int span = app_view_rows(app, start);
            bool image_selected = ed->cursor >= image->start && ed->cursor <= image->end;
            draw_image(app, image, pixels, width, height, stride, padding + gutter, y + 2,
                       width - 2 * padding - gutter, (span - 1) * line_height - 4,
                       image_selected || (selected && start >= selection_a && start < selection_b));
            row += span - 1;
            if (next <= start || next == ed->length)
                break;
            start = next;
            continue;
        }
        size_t offset = start;
        unsigned column = 0;
        while (offset < end) {
            size_t at = offset;
            uint32_t cp = editor_codepoint(ed->text, end, &offset);
            unsigned advance = cp == '\t' ? 4 - column % 4 : 1, style = 0;
            for (size_t i = 0; i < span_count; ++i)
                if (at >= spans[i].start && at < spans[i].end)
                    style |= spans[i].style;
            int x = padding + gutter + (int)column * cell;
            uint32_t color = theme->text;
            if (style & MD_DIM)
                color = theme->dim;
            else if (style & MD_LINK)
                color = theme->link;
            else if (style & MD_HEADING)
                color = theme->heading;
            else if (style & MD_QUOTE)
                color = theme->quote;
            else if (style & MD_DONE)
                color = theme->done;
            else if (style & MD_ITALIC)
                color = theme->italic;
            if (style & MD_CODE)
                rectangle(pixels, width, height, stride, x, y, cell * (int)advance, line_height,
                          theme->code);
            if (highlight(ed, at))
                rectangle(pixels, width, height, stride, x, y, cell * (int)advance, line_height,
                          theme->search);
            if (selected && at >= selection_a && at < selection_b)
                rectangle(pixels, width, height, stride, x, y, cell * (int)advance, line_height,
                          theme->selection);
            if (at == ed->cursor) {
                cursor_x = x;
                cursor_y = y;
            }
            if (cp != '\t') {
                if (cp < 32 || cp == 127)
                    cp = '?';
                if (!glyph(app, pixels, width, height, stride, cp, x, y + ascender, status_y,
                           color))
                    return false;
                if ((style & MD_BOLD) && !glyph(app, pixels, width, height, stride, cp, x + 1,
                                                y + ascender, status_y, color))
                    return false;
                if (style & MD_DONE)
                    rectangle(pixels, width, height, stride, x, y + ascender / 2, cell, 1, color);
            }
            column += advance;
        }
        if (ed->cursor == end && (end == ed->length || ed->text[end] == '\n')) {
            cursor_x = padding + gutter + (int)column * cell;
            cursor_y = y;
        }
        if (next == start || (next == ed->length && (next == 0 || ed->text[next - 1] != '\n')))
            break;
        start = next;
    }
    if (!ed->command_active && cursor_x >= 0) {
        if (ed->mode == INSERT)
            rectangle(pixels, width, height, stride, cursor_x, cursor_y, 2, line_height,
                      theme->cursor);
        else
            rectangle(pixels, width, height, stride, cursor_x, cursor_y + line_height - 3, cell, 3,
                      theme->cursor);
    }
    rectangle(pixels, width, height, stride, 0, status_y, width, line_height + 2 * padding,
              theme->status);
    char status[512];
    const char *mode = ed->mode == INSERT        ? "INSERT"
                       : ed->mode == VISUAL      ? "VISUAL"
                       : ed->mode == VISUAL_LINE ? "VISUAL LINE"
                                                 : "NORMAL";
    const char *filename = ed->filename ? leaf_name(ed->filename) : "[No Name]";
    int baseline = status_y + padding + ascender;
    if (ed->command_active) {
        char prompt[1100];
        snprintf(prompt, sizeof(prompt), "%c%s_", ed->command_kind ? ed->command_kind : ':',
                 ed->command);
        if (!label(app, pixels, width, height, stride, prompt, padding, baseline, cell))
            return false;
    } else {
        char position[64];
        snprintf(position, sizeof(position), "%zu:%zu", editor_line_number(ed, ed->cursor) + 1,
                 editor_column(ed, ed->cursor) + 1);
        int position_x = width - padding - (int)strlen(position) * cell;
        snprintf(status, sizeof(status), "%s  %s%s%s%s", mode, filename,
                 ed->modified ? " [+]" : "", ed->pending[0] ? "  " : "", ed->pending);
        if (app->work_at && !ed->message[0]) {
            size_t n = strlen(status);
            snprintf(status + n, sizeof(status) - n, "  indexing image");
        }
        if (!label(app, pixels, position_x - cell, height, stride,
                   ed->message[0] ? ed->message : status, padding, baseline, cell) ||
            !label(app, pixels, width, height, stride, position, position_x, baseline, cell))
            return false;
    }
    if (app->client_decorated && !app->maximized) {
        /* A small grip also exposes the resize corner without cursor-shape support. */
        int stroke = (int)((app->scale120 + 119) / 120);
        for (int stripe = 1; stripe <= 3; ++stripe) {
            int length = stripe * padding / 3;
            for (int i = 0; i < length; ++i)
                rectangle(pixels, width, height, stride, width - 3 * stroke - i,
                          height - 3 * stroke - length + 1 + i, stroke, stroke, theme->dim);
        }
    }
    if (app->context_menu) {
        rectangle(pixels, width, height, stride, app->context_x, app->context_y, 26 * cell,
                  2 * line_height, theme->menu);
        rectangle(pixels, width, height, stride, app->context_x,
                  app->context_y + app->context_choice * line_height, 26 * cell, line_height,
                  theme->menu_selected);
        if (!label(app, pixels, width, height, stride, " Copy Text from Image", app->context_x,
                   app->context_y + ascender, cell))
            return false;
        if (!label(app, pixels, width, height, stride, " Open Image", app->context_x,
                   app->context_y + line_height + ascender, cell))
            return false;
    }
    return true;
}
size_t app_hit_test(struct app *app, int logical_x, int logical_y)
{
    if (app->cell < 1 || app->line_height < 1)
        return app->editor.cursor;
    int x = (int)((int64_t)logical_x * app->scale120 / 120) - app->padding - app->gutter;
    int y = (int)((int64_t)logical_y * app->scale120 / 120) - app->padding - app->title_height;
    unsigned row = y > 0 ? (unsigned)(y / app->line_height) : 0;
    if (row >= app->editor.viewport_rows)
        row = app->editor.viewport_rows - 1;
    struct editor *ed = &app->editor;
    size_t p = ed->top;
    while (row) {
        unsigned span = (unsigned)app_view_rows(app, p);
        if (row < span)
            return p;
        row -= span;
        size_t next = app_view_next(app, p);
        if (next == p)
            break;
        p = next;
    }
    size_t end = app_view_next(app, p);
    unsigned col = 0, target = x > 0 ? (unsigned)(x / app->cell) : 0;
    while (p < end && ed->text[p] != '\n') {
        unsigned step = ed->text[p] == '\t' ? 4 - col % 4 : 1;
        if (col + step > target)
            break;
        col += step;
        p = editor_next(ed, p);
    }
    return p;
}
bool app_paint(struct app *app, uint32_t *pixels, int width, int height, int stride)
{
    if (!pixels || width < 1 || height < 1 || stride < width)
        return false;
    if (!app->font)
        app->font = tiny_font_open();
    if (!app->font)
        return false;
    bool ok = paint(app, pixels, width, height, stride);
    tiny_font_end_frame(app->font);
    return ok;
}
