#include "app.h"
#include "markdown.h"
#include "media.h"
#include "render-internal.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "colors.h"
static const struct palette *colors(const struct app *app)
{
    switch (app->editor.colorscheme) {
    case COLOR_TOKYONIGHT:
        return app->editor.light ? &theme_tokyonight_light : &theme_tokyonight_dark;
    case COLOR_CATPPUCCIN:
        return app->editor.light ? &theme_catppuccin_light : &theme_catppuccin_dark;
    case COLOR_KANAGAWA:
        return app->editor.light ? &theme_kanagawa_light : &theme_kanagawa_dark;
    case COLOR_EVERFOREST:
        return app->editor.light ? &theme_everforest_light : &theme_everforest_dark;
    default:
        return app->editor.light ? &light : &dark;
    }
}

uint32_t render_blend(uint32_t background, unsigned alpha, uint32_t foreground)
{
    uint32_t pixel = 0xff000000;
    for (unsigned shift = 0; shift < 24; shift += 8) {
        unsigned fg = (foreground >> shift) & 255;
        unsigned bg = (background >> shift) & 255;
        pixel |= ((fg * alpha + bg * (255 - alpha) + 127) / 255) << shift;
    }
    return pixel;
}

void render_rectangle(uint32_t *pixels, int width, int height, int stride, int x, int y, int w,
                      int h, uint32_t color)
{
    for (int row = y < 0 ? 0 : y; row < height && row < y + h; ++row)
        for (int col = x < 0 ? 0 : x; col < width && col < x + w; ++col)
            pixels[(size_t)row * (size_t)stride + (size_t)col] = color;
}

/* A filled w×h box with corners of the given radius. */
void render_rounded_box(uint32_t *pixels, int width, int height, int stride, int x, int y, int w,
                        int h, int radius, uint32_t color)
{
    if (radius > w / 2)
        radius = w / 2;
    if (radius > h / 2)
        radius = h / 2;
    for (int row = 0; row < h; ++row) {
        int dy = row < radius ? radius - row - 1 : row >= h - radius ? row - (h - radius) : 0;
        int inset = 0;
        while (inset < radius && (radius - inset) * (radius - inset) + dy * dy > radius * radius)
            ++inset;
        render_rectangle(pixels, width, height, stride, x + inset, y + row, w - 2 * inset, 1,
                         color);
    }
}

static void rounded_rectangle(uint32_t *pixels, int width, int height, int stride, int x, int y,
                              int size, int radius, uint32_t color)
{
    render_rounded_box(pixels, width, height, stride, x, y, size, size, radius, color);
}

static bool styled_glyph(struct app *app, uint32_t *pixels, int width, int height, int stride,
                         uint32_t cp, int x, int baseline, int clip_bottom, uint32_t color,
                         unsigned style)
{
    const struct tiny_glyph *bitmap = tiny_font_glyph(app->font, cp);
    if (!bitmap)
        return false;
    for (int row = 0; row < bitmap->height; ++row) {
        int y = baseline - bitmap->top + row;
        if (y < 0 || y >= height || y >= clip_bottom)
            continue;
        /* Shear the cached regular glyph; no additional font or bitmap cache. */
        int slant = style & MD_ITALIC ? (bitmap->top - row) / 4 : 0;
        const unsigned char *coverage = bitmap->alpha + (size_t)row * bitmap->width;
        for (int col = 0; col < bitmap->width; ++col) {
            int px = x + bitmap->left + col + slant;
            if (px < 0 || px >= width)
                continue;
            size_t offset = (size_t)y * (size_t)stride + (size_t)px;
            pixels[offset] = render_blend(pixels[offset], coverage[col], color);
        }
    }
    return true;
}

static bool glyph(struct app *app, uint32_t *pixels, int width, int height, int stride, uint32_t cp,
                  int x, int baseline, int clip_bottom, uint32_t color)
{
    return styled_glyph(app, pixels, width, height, stride, cp, x, baseline, clip_bottom, color, 0);
}

bool render_label(struct app *app, uint32_t *pixels, int width, int height, int stride,
                  const char *text, int x, int baseline, int cell, uint32_t color)
{
    size_t length = strlen(text), offset = 0;
    while (offset < length && x < width - cell) {
        uint32_t cp = editor_codepoint(text, length, &offset);
        if (cp < 32 || cp == 127)
            cp = '?';
        if (!glyph(app, pixels, width, height, stride, cp, x, baseline, height, color))
            return false;
        x += cell;
    }
    return true;
}

static void title_bar(struct app *app, uint32_t *pixels, int width, int height, int stride)
{
    const struct palette *theme = colors(app);
    int h = app->title_height;
    render_rectangle(pixels, width, height, stride, 0, 0, width, h, theme->title);
    render_rectangle(pixels, width, height, stride, 0, h - 1, width, 1, theme->title_separator);
    int size = h / 4, stroke = (int)(app->scale120 / 120);
    if (size < 3)
        size = 3;
    if (stroke < 1)
        stroke = 1;
    for (int button = 0; button < 3; ++button) {
        enum app_title_action action = TITLE_MINIMIZE + button;
        bool active = app->title_hover == action &&
                      (app->title_pressed == TITLE_NONE || app->title_pressed == action);
        uint32_t foreground = theme->text;
        if (active) {
            bool pressed = app->title_pressed == action;
            uint32_t background = pressed ? theme->title_pressed : theme->title_hover;
            if (action == TITLE_CLOSE) {
                background = pressed ? 0xffa83333 : 0xffc74242;
                foreground = 0xffffffff;
            }
            int inset = 3 * stroke;
            rounded_rectangle(pixels, width, height, stride, width - (3 - button) * h + inset,
                              inset, h - 2 * inset, 3 * stroke, background);
        }
        int x = width - (3 - button) * h + (h - size) / 2, y = (h - size) / 2;
        if (button == 0)
            render_rectangle(pixels, width, height, stride, x, y + size, size, stroke, foreground);
        else if (button == 1) {
            int offset = app->maximized ? 3 * stroke : 0;
            render_rectangle(pixels, width, height, stride, x + offset, y - offset, size, stroke,
                             foreground);
            render_rectangle(pixels, width, height, stride, x + size + offset - stroke, y - offset,
                             stroke, size, foreground);
            render_rectangle(pixels, width, height, stride, x, y, size, stroke, foreground);
            render_rectangle(pixels, width, height, stride, x, y, stroke, size, foreground);
            render_rectangle(pixels, width, height, stride, x + size - stroke, y, stroke, size,
                             foreground);
            render_rectangle(pixels, width, height, stride, x, y + size - stroke, size, stroke,
                             foreground);
        } else
            for (int i = 0; i < size; ++i) {
                render_rectangle(pixels, width, height, stride, x + i, y + i, stroke, stroke,
                                 foreground);
                render_rectangle(pixels, width, height, stride, x + size - i - 1, y + i, stroke,
                                 stroke, foreground);
            }
    }
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
static struct md_cache *markdown_at(struct app *app, size_t position)
{
    struct editor *ed = &app->editor;
    struct md_cache *cache = &app->markdown;
    size_t start = editor_line_start(ed, position);
    if (cache->valid && cache->revision == ed->revision && cache->start == start)
        return cache;
    size_t p = 0;
    char fence = 0;
    if (cache->valid && cache->revision == ed->revision && start > cache->start) {
        p = cache->end < ed->length ? cache->end + 1 : cache->end;
        fence = cache->fence_after;
    }
    while (p < start) {
        char marker = editor_fence(ed, p);
        if (marker)
            fence = fence ? (fence == marker ? 0 : fence) : marker;
        size_t end = editor_line_end(ed, p);
        p = end < ed->length ? end + 1 : end;
    }
    bool table_continuation = cache->valid && cache->revision == ed->revision &&
                              cache->end < ed->length && cache->end + 1 == start;
    char marker = editor_fence(ed, start);
    cache->start = start;
    cache->end = editor_line_end(ed, start);
    cache->revision = ed->revision;
    cache->valid = true;
    cache->fenced = fence || marker;
    cache->fence_line = marker && (!fence || marker == fence);
    cache->fence_after = marker ? (fence ? (fence == marker ? 0 : fence) : marker) : fence;
    if (cache->fenced)
        cache->table.count = 0;
    else
        markdown_table(ed->text, ed->length, start, cache->end, &cache->table, table_continuation);
    cache->count = markdown_line(ed->text, start, cache->end, cache->spans, 128, cache->fenced);
    if (cache->table.count) {
        size_t previous = start;
        for (unsigned i = 0; i < cache->table.count; ++i) {
            struct md_cell cell = cache->table.cells[i];
            if (previous < cell.start && cache->count < 128)
                cache->spans[cache->count++] = (struct md_span){previous, cell.start, MD_DIM};
            if (cache->table.header && cell.start < cell.end && cache->count < 128)
                cache->spans[cache->count++] = (struct md_span){cell.start, cell.end, MD_BOLD};
            previous = cell.end;
        }
        if (previous < cache->end && cache->count < 128)
            cache->spans[cache->count++] = (struct md_span){previous, cache->end, MD_DIM};
        if (cache->table.separator && cache->count < 128)
            cache->spans[cache->count++] = (struct md_span){start, cache->end, MD_DIM};
    }
    struct md_block block = markdown_block(ed->text, start, cache->end);
    cache->indent = cache->fenced ? 0 : block.indent;
    cache->heading = cache->fenced ? 0 : block.heading;
    cache->rule = !cache->fenced && block.rule;
    cache->quote = false;
    for (size_t i = 0; i < cache->count; ++i)
        cache->quote |= (cache->spans[i].style & MD_QUOTE) != 0;
    return cache;
}
static unsigned markdown_style(const struct md_cache *cache, size_t at)
{
    unsigned style = 0;
    for (size_t i = 0; i < cache->count; ++i)
        if (at >= cache->spans[i].start && at < cache->spans[i].end)
            style |= cache->spans[i].style;
    return style;
}
struct text_layout {
    unsigned size;
    int cell, ascender, height, inset;
};
unsigned render_font_size(const struct app *app)
{
    return (unsigned)(((app->font_size ? app->font_size : 18) * 64 * (int64_t)app->scale120 + 60) /
                      120);
}
static struct text_layout text_layout(const struct app *app, const struct md_cache *cache)
{
    unsigned size = render_font_size(app);
    struct text_layout layout = {size, app->cell, tiny_font_metrics_at(size).ascender,
                                 app->line_height, 0};
    if (!cache || !cache->heading || cache->table.count)
        return layout;
    static const unsigned reading[] = {190, 160, 135, 118, 108, 100};
    static const unsigned normal[] = {135, 125, 116, 110, 105, 100};
    bool reader = app->editor.view == VIEW_READING;
    unsigned ratio = (reader ? reading : normal)[cache->heading - 1];
    layout.size = (size * ratio + 50) / 100;
    if (layout.size > 128 * 64)
        layout.size = 128 * 64;
    struct tiny_font_metrics metrics = tiny_font_metrics_at(layout.size);
    layout.cell = metrics.advance;
    layout.ascender = metrics.ascender;
    layout.inset = reader ? (int)((3 * app->scale120 + 60) / 120) : 0;
    layout.height = metrics.line_height + 2 + layout.inset * 2;
    if (layout.height < app->line_height)
        layout.height = app->line_height;
    return layout;
}
/* Shared table geometry keeps wrapping, drawing and clicks on source offsets. */
static size_t table_segment(const struct app *app, const struct md_cache *cache, size_t start,
                            size_t end, unsigned width, unsigned *used)
{
    size_t p = start, space = SIZE_MAX;
    unsigned space_used = 0;
    *used = 0;
    while (p < end) {
        size_t at = p;
        uint32_t cp = editor_codepoint(app->editor.text, end, &p);
        if (markdown_style(cache, at) & MD_HIDE)
            continue;
        unsigned step = cp == '\t' ? 4 - *used % 4 : 1;
        if (*used + step > width && *used) {
            if (space != SIZE_MAX) {
                *used = space_used;
                return space;
            }
            return at;
        }
        *used += step > width ? width : step;
        if (cp == ' ' || cp == '\t') {
            space = p;
            space_used = *used;
        }
    }
    return p;
}
static int table_rows(const struct app *app, const struct md_cache *cache, unsigned width)
{
    int rows = 1;
    for (unsigned i = 0; i < cache->table.count; ++i) {
        struct md_cell cell = cache->table.cells[i];
        size_t p = cell.start;
        int count = 0;
        do {
            unsigned used;
            size_t next = table_segment(app, cache, p, cell.end, width, &used);
            ++count;
            if (count > (int)app->editor.viewport_rows || next <= p)
                break;
            p = next;
        } while (p < cell.end);
        if (count > rows)
            rows = count;
    }
    return rows;
}
static unsigned table_width(const struct app *app, const struct md_cache *cache)
{
    unsigned count = cache->table.count, columns = app->editor.viewport_columns;
    if (app->editor.view != VIEW_READING || !count || columns < 4 * count + 1)
        return 0;
    unsigned width = (columns - 1) / count - 3;
    return cache->table.separator || table_rows(app, cache, width) <= (int)app->editor.viewport_rows
               ? width
               : 0;
}
static unsigned table_align(const struct md_cache *cache, unsigned column, unsigned width,
                            unsigned used)
{
    unsigned spare = width > used ? width - used : 0;
    return cache->table.align[column] == 2   ? spare
           : cache->table.align[column] == 1 ? spare / 2
                                             : 0;
}
/* A hanging indent is capped so even deeply nested lists make progress. */
static unsigned view_indent(const struct app *app, const struct md_cache *cache, size_t start)
{
    unsigned columns = app->editor.viewport_columns;
    return start == cache->start ? 0 : cache->indent < columns / 2 ? cache->indent : columns / 2;
}
static const struct media_entry *view_image(struct app *app, size_t start)
{
    if (app->editor.view == VIEW_SOURCE ||
        (app->editor.view == VIEW_READING && markdown_at(app, start)->fenced))
        return NULL;
    const struct media_entry *image = media_at(app, start);
    return image && image->width > 0 ? image : NULL;
}
size_t app_view_next(struct app *app, size_t start)
{
    const struct media_entry *image = view_image(app, start);
    if (image)
        return image->end < app->editor.length ? image->end + 1 : image->end;
    unsigned columns = app->editor.viewport_columns ? app->editor.viewport_columns : 80;
    if (app->editor.view == VIEW_SOURCE)
        return app_row_next(&app->editor, start, columns);
    struct md_cache *cache = markdown_at(app, start);
    struct text_layout layout = text_layout(app, cache);
    if (layout.cell > 0 && app->text_width > 0) {
        columns = (unsigned)(app->text_width / layout.cell);
        if (!columns)
            columns = 1;
    }
    if (app->editor.view == VIEW_NORMAL)
        return app_row_next(&app->editor, start, columns);
    if (table_width(app, cache))
        return cache->end < app->editor.length ? cache->end + 1 : cache->end;
    size_t p = start, last_space = SIZE_MAX;
    unsigned col = view_indent(app, cache, start);
    while (!cache->fence_line && !cache->rule && p < cache->end) {
        size_t at = p;
        uint32_t cp = editor_codepoint(app->editor.text, cache->end, &p);
        if (markdown_style(cache, at) & MD_HIDE)
            continue;
        unsigned step = cp == '\t' ? 4 - col % 4 : 1;
        if (col + step > columns)
            return last_space != SIZE_MAX && last_space > start ? last_space : col ? at : p;
        col += step;
        if (cp == ' ' || cp == '\t')
            last_space = p;
    }
    return cache->end < app->editor.length ? cache->end + 1 : cache->end;
}
int app_view_height(struct app *app, size_t start)
{
    if (app->editor.view == VIEW_SOURCE)
        return app->line_height;
    struct md_cache *cache = markdown_at(app, start);
    if (app->editor.view == VIEW_READING && cache->fence_line)
        return 0;
    if (!view_image(app, start)) {
        unsigned width = table_width(app, cache);
        if (width)
            return cache->table.separator ? 0 : table_rows(app, cache, width) * app->line_height;
        return text_layout(app, cache).height;
    }
    return media_rows(app, start, app->text_width, app->line_height ? app->line_height : 24,
                      app->editor.viewport_rows ? app->editor.viewport_rows : 24) *
           app->line_height;
}
int app_view_rows(struct app *app, size_t start)
{
    int line = app->line_height ? app->line_height : 24;
    return (app_view_height(app, start) + line - 1) / line;
}
static void draw_image(struct app *app, const struct media_entry *entry, uint32_t *pixels,
                       int width, int height, int stride, int x, int y, int available_width,
                       int available_height, bool selected)
{
    const struct palette *theme = colors(app);
    struct image image = {0};
    if (available_width < 1 || available_height < 1 || !media_pixels(app, entry, &image))
        return;
    int w, h;
    media_display_size(app, entry, available_width, &w, &h);
    if (w < 1 || h < 1) {
        w = image.width;
        h = image.height;
        if (w > available_width) {
            h = (int)((int64_t)h * available_width / w);
            w = available_width;
        }
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
        render_rectangle(pixels, width, height, stride, x - 3, y - 3, w + 6, h + 6, theme->link);
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
            pixels[at] = render_blend(pixels[at], p[3], color);
        }
    media_highlight(app, entry, pixels, width, height, stride, x, y, w, h);
    if (selected && media_at(app, app->editor.cursor) == entry) {
        app->image_box = true;
        app->image_box_start = entry->start;
        app->image_box_x = x;
        app->image_box_y = y;
        app->image_box_w = w;
        app->image_box_h = h;
    }
    if (selected) {
        /* Corner handles keep selection visible against light or busy images. */
        for (int corner = 0; corner < 4; ++corner)
            render_rectangle(pixels, width, height, stride, (corner & 1) ? x + w - 3 : x - 3,
                             (corner & 2) ? y + h - 3 : y - 3, 6, 6, theme->handle);
    }
    image_free(&image);
}
static bool highlight(const struct editor *ed, size_t offset)
{
    if (ed->search_hidden || ed->mode == INSERT || !ed->search[0])
        return false;
    size_t n = strlen(ed->search), begin = offset >= n - 1 ? offset - n + 1 : 0;
    for (size_t p = begin; p <= offset && p + n <= ed->length; ++p)
        if (editor_match_at(ed, p))
            return true;
    return false;
}
static uint32_t inline_color(const struct palette *theme, unsigned style)
{
    if (style & MD_DIM)
        return theme->dim;
    if (style & MD_LINK)
        return theme->link;
    if (style & MD_HEADING)
        return theme->heading;
    if (style & MD_QUOTE)
        return theme->quote;
    if (style & MD_DONE)
        return theme->done;
    if (style & MD_ITALIC)
        return theme->italic;
    return theme->text;
}
static bool draw_table(struct app *app, struct md_cache *cache, uint32_t *pixels, int width,
                       int height, int stride, int y, int ascender, int limit, unsigned content,
                       size_t selection_a, size_t selection_b)
{
    const struct palette *theme = colors(app);
    int rows = table_rows(app, cache, content), cell = app->cell;
    int table_height = rows * app->line_height;
    int column_width = (int)(content + 3) * cell;
    int table_width = column_width * (int)cache->table.count;
    if (y + table_height > limit)
        table_height = limit - y;
    if (cache->table.header) {
        render_rectangle(pixels, width, height, stride, app->text_x, y, table_width, table_height,
                         theme->code);
        render_rectangle(pixels, width, height, stride, app->text_x, y, table_width, 1, theme->dim);
    }
    render_rectangle(pixels, width, height, stride, app->text_x, y + table_height - 1, table_width,
                     1, theme->dim);
    for (unsigned i = 0; i <= cache->table.count; ++i)
        render_rectangle(pixels, width, height, stride, app->text_x + (int)i * column_width, y, 1,
                         table_height, theme->dim);
    for (unsigned i = 0; i < cache->table.count; ++i) {
        struct md_cell range = cache->table.cells[i];
        size_t p = range.start;
        for (int row = 0; p < range.end && y + row * app->line_height < limit; ++row) {
            unsigned used;
            size_t next = table_segment(app, cache, p, range.end, content, &used);
            unsigned column = table_align(cache, i, content, used);
            int top = y + row * app->line_height;
            while (p < next) {
                size_t at = p;
                uint32_t cp = editor_codepoint(app->editor.text, next, &p);
                unsigned style = markdown_style(cache, at);
                if (style & MD_HIDE)
                    continue;
                int x = app->text_x + (int)i * column_width + ((int)column + 1) * cell;
                unsigned advance =
                    cp == '\t' ? 4 - (column - table_align(cache, i, content, used)) % 4 : 1;
                if (advance > content)
                    advance = content;
                uint32_t color = inline_color(theme, style);
                if (style & MD_CODE)
                    render_rectangle(pixels, width, height, stride, x, top, (int)advance * cell,
                                     app->line_height, theme->code);
                if (highlight(&app->editor, at))
                    render_rectangle(pixels, width, height, stride, x, top, (int)advance * cell,
                                     app->line_height, theme->search);
                if (at >= selection_a && at < selection_b)
                    render_rectangle(pixels, width, height, stride, x, top, (int)advance * cell,
                                     app->line_height, theme->selection);
                if (highlight(&app->editor, at))
                    color = theme->search_text;
                if (at >= selection_a && at < selection_b)
                    color = theme->selection_text;
                if (cp != '\t') {
                    if (cp < 32 || cp == 127)
                        cp = '?';
                    if (!styled_glyph(app, pixels, width, height, stride, cp, x, top + ascender,
                                      limit, color, style))
                        return false;
                    if ((cache->table.header || (style & MD_BOLD)) &&
                        !styled_glyph(app, pixels, width, height, stride, cp, x + 1, top + ascender,
                                      limit, color, style))
                        return false;
                    if (style & (MD_DONE | MD_STRIKE))
                        render_rectangle(pixels, width, height, stride, x, top + ascender * 3 / 4,
                                         cell, (int)((app->scale120 + 119) / 120), color);
                }
                column += advance;
            }
        }
    }
    return true;
}
static size_t table_hit(const struct app *app, const struct md_cache *cache, int x, unsigned row,
                        unsigned content)
{
    unsigned target = x > 0 ? (unsigned)(x / app->cell) : 0;
    unsigned column = target / (content + 3);
    if (column >= cache->table.count)
        column = cache->table.count - 1;
    struct md_cell cell = cache->table.cells[column];
    size_t p = cell.start, end = p;
    unsigned used = 0;
    for (unsigned i = 0; i <= row; ++i) {
        end = table_segment(app, cache, p, cell.end, content, &used);
        if (i < row && end == cell.end)
            return cell.end;
        if (i == row)
            break;
        p = end;
    }
    unsigned left = column * (content + 3) + 1 + table_align(cache, column, content, used);
    target = target > left ? target - left : 0;
    unsigned col = 0;
    while (p < end) {
        size_t at = p;
        uint32_t cp = editor_codepoint(app->editor.text, end, &p);
        if (markdown_style(cache, at) & MD_HIDE)
            continue;
        unsigned step = cp == '\t' ? 4 - col % 4 : 1;
        if (col + step > target)
            return at;
        col += step;
    }
    return end;
}
static bool paint(struct app *app, uint32_t *pixels, int width, int height, int stride)
{
    struct editor *ed = &app->editor;
    const struct palette *theme = colors(app);
    unsigned size = render_font_size(app);
    if (!tiny_font_set_size(app->font, size))
        return false;
    struct tiny_font_metrics metrics = tiny_font_metrics(app->font);
    bool reading = ed->view == VIEW_READING;
    int cell = metrics.advance, ascender = metrics.ascender;
    int status_line = metrics.line_height + 2, line_height = status_line;
    if (reading)
        line_height += (int)((4 * (int64_t)app->scale120 + 60) / 120);
    int padding = (int)((8 * (int64_t)app->scale120 + 60) / 120);
    app->title_height = app->client_decorated ? status_line + padding : 0;
    int top = app->title_height + padding;
    int status_y = height - status_line - 2 * padding, rows = (status_y - top) / line_height;
    size_t lines = editor_line_count(ed);
    int digits = 1;
    for (size_t n = lines; n >= 10; n /= 10)
        ++digits;
    if (digits < 4)
        digits = 4;
    bool numbers = !reading && (ed->number || ed->relativenumber);
    int gutter = numbers ? (digits + 2) * cell : 0;
    int text_x = padding + gutter, text_width = width - 2 * padding - gutter;
    if (reading) {
        int margin = (int)((24 * (int64_t)app->scale120 + 60) / 120);
        text_width = width - 2 * margin;
        if (text_width > 80 * cell)
            text_width = 80 * cell;
        if (text_width < cell)
            text_width = cell;
        text_x = (width - text_width) / 2;
    }
    int columns = text_width / cell;
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
    app->text_x = text_x;
    app->text_width = text_width;
    app->text_height = status_y - top;
    if (ed->top > ed->length)
        ed->top = editor_line_start(ed, ed->length);
    if (app->follow_cursor) {
        if (ed->cursor < ed->top)
            ed->top = editor_line_start(ed, ed->cursor);
        size_t p = ed->top, cursor_offset = 0;
        while (p < ed->cursor) {
            size_t next = app_view_next(app, p);
            if (next <= p || next > ed->cursor ||
                (next == ed->cursor && ed->text[next - 1] != '\n' && next == ed->length))
                break;
            cursor_offset += (size_t)app_view_height(app, p);
            p = next;
        }
        size_t cursor_height = (size_t)app_view_height(app, p);
        size_t available = app->text_height > 0 ? (size_t)app->text_height : 1;
        if (cursor_height > available)
            cursor_height = available;
        size_t remove = cursor_offset + cursor_height > available
                            ? cursor_offset + cursor_height - available
                            : 0;
        while (remove && ed->top < p) {
            size_t step = (size_t)app_view_height(app, ed->top), next = app_view_next(app, ed->top);
            if (next == ed->top)
                break;
            ed->top = next;
            remove = remove > step ? remove - step : 0;
        }
    }
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            pixels[(size_t)y * stride + x] = theme->background;
    if (app->client_decorated)
        title_bar(app, pixels, width, height, stride);
    size_t selection_a = 0, selection_b = 0;
    bool linewise;
    bool selected = editor_selection(ed, &selection_a, &selection_b, &linewise);
    size_t start = ed->top, cursor_line = editor_line_number(ed, ed->cursor);
    int cursor_x = -1, cursor_y = -1, cursor_height = line_height, cursor_cell = cell;
    for (int y = top; y < status_y;) {
        size_t next = app_view_next(app, start), end = next;
        if (end > start && ed->text[end - 1] == '\n')
            --end;
        size_t logical_start = editor_line_start(ed, start);
        struct md_cache *cache = ed->view == VIEW_SOURCE ? NULL : markdown_at(app, start);
        if (reading && (cache->fence_line || (table_width(app, cache) && cache->table.separator))) {
            if (next <= start)
                break;
            start = next;
            continue;
        }
        if (numbers && start == logical_start) {
            /* Like Vim: relativenumber counts lines from the cursor, whose own line shows 0,
             * or its number left-aligned when number is also set. */
            char number[32];
            size_t line = editor_line_number(ed, start);
            bool current = line == cursor_line;
            if (!ed->relativenumber)
                snprintf(number, sizeof(number), "%*zu", digits, line + 1);
            else if (current && ed->number)
                snprintf(number, sizeof(number), "%-*zu", digits, line + 1);
            else
                snprintf(number, sizeof(number), "%*zu", digits,
                         current              ? 0
                         : line > cursor_line ? line - cursor_line
                                              : cursor_line - line);
            uint32_t color = current ? theme->text : theme->dim;
            for (int i = 0; number[i]; ++i)
                if (!glyph(app, pixels, width, height, stride, (unsigned char)number[i],
                           padding + i * cell, y + ascender, status_y, color))
                    return false;
        }
        const struct media_entry *image = view_image(app, start);
        if (image) {
            int image_height = app_view_height(app, start);
            bool image_selected = ed->cursor >= image->start && ed->cursor <= image->end;
            draw_image(app, image, pixels, width, height, stride, text_x, y + 2, text_width,
                       image_height - line_height - 4,
                       image_selected || (selected && start >= selection_a && start < selection_b));
            y += image_height;
            if (next <= start || next == ed->length)
                break;
            start = next;
            continue;
        }
        unsigned table_content = reading ? table_width(app, cache) : 0;
        if (table_content) {
            int span = table_rows(app, cache, table_content);
            if (!draw_table(app, cache, pixels, width, height, stride, y, ascender, status_y,
                            table_content, selected ? selection_a : 0, selected ? selection_b : 0))
                return false;
            y += span * line_height;
            if (next <= start || next == ed->length)
                break;
            start = next;
            continue;
        }
        if (cache && cache->fenced)
            render_rectangle(pixels, width, height, stride, text_x, y, text_width, line_height,
                             theme->code);
        if (reading && cache->quote)
            render_rectangle(pixels, width, height, stride, text_x - padding, y, 2, line_height,
                             theme->quote);
        struct text_layout layout = text_layout(app, cache);
        int row_height = layout.height, text_y = y + layout.inset;
        int text_cell = layout.cell, text_ascender = layout.ascender;
        if (!tiny_font_set_size(app->font, layout.size))
            return false;
        bool last = cache && next >= cache->end;
        if (reading && (cache->rule || (cache->heading && cache->heading <= 2 && last))) {
            int rule_y = cache->rule ? y + line_height / 2 : y + row_height - 2;
            render_rectangle(pixels, width, height, stride, text_x, rule_y, text_width,
                             cache->heading == 1 ? 2 : 1, theme->dim);
        }
        size_t offset = start;
        unsigned column = reading ? view_indent(app, cache, start) : 0;
        if (reading && cache->rule)
            offset = end;
        while (offset < end) {
            size_t at = offset;
            uint32_t cp = editor_codepoint(ed->text, end, &offset);
            unsigned style = cache ? markdown_style(cache, at) : 0;
            if (reading && (style & MD_HIDE))
                continue;
            unsigned advance = cp == '\t' ? 4 - column % 4 : 1;
            int x = text_x + (int)column * text_cell;
            uint32_t color = inline_color(theme, style);
            if (style & MD_CODE)
                render_rectangle(pixels, width, height, stride, x, y, text_cell * (int)advance,
                                 row_height, theme->code);
            if (highlight(ed, at))
                render_rectangle(pixels, width, height, stride, x, y, text_cell * (int)advance,
                                 row_height, theme->search);
            if (selected && at >= selection_a && at < selection_b)
                render_rectangle(pixels, width, height, stride, x, y, text_cell * (int)advance,
                                 row_height, theme->selection);
            if (highlight(ed, at))
                color = theme->search_text;
            if (selected && at >= selection_a && at < selection_b)
                color = theme->selection_text;
            if (at == ed->cursor) {
                cursor_x = x;
                cursor_y = y;
                cursor_height = row_height;
                cursor_cell = text_cell;
            }
            if (reading && (style & (MD_BULLET | MD_TASK))) {
                int stroke = (int)((app->scale120 + 119) / 120);
                int center = text_y + text_ascender * 2 / 3;
                if (style & MD_BULLET) {
                    render_rectangle(pixels, width, height, stride, x + text_cell / 2 - stroke,
                                     center - stroke, 2 * stroke + 1, 2 * stroke + 1, theme->dim);
                } else {
                    int side = text_cell - 2 * stroke, left = x + stroke, upper = center - side / 2;
                    uint32_t ink = style & MD_CHECKED ? theme->quote : theme->dim;
                    render_rectangle(pixels, width, height, stride, left, upper, side, stroke, ink);
                    render_rectangle(pixels, width, height, stride, left, upper, stroke, side, ink);
                    render_rectangle(pixels, width, height, stride, left + side - stroke, upper,
                                     stroke, side, ink);
                    render_rectangle(pixels, width, height, stride, left, upper + side - stroke,
                                     side, stroke, ink);
                    if (style & MD_CHECKED)
                        render_rectangle(pixels, width, height, stride, left + 2 * stroke,
                                         upper + 2 * stroke, side - 4 * stroke, side - 4 * stroke,
                                         ink);
                }
            } else if (cp != '\t') {
                if (cp < 32 || cp == 127)
                    cp = '?';
                if (!styled_glyph(app, pixels, width, height, stride, cp, x, text_y + text_ascender,
                                  status_y, color, style))
                    return false;
                if ((style & MD_BOLD) &&
                    !styled_glyph(app, pixels, width, height, stride, cp, x + 1,
                                  text_y + text_ascender, status_y, color, style))
                    return false;
                if (style & (MD_DONE | MD_STRIKE))
                    render_rectangle(pixels, width, height, stride, x,
                                     text_y + text_ascender * 3 / 4, text_cell,
                                     (int)((app->scale120 + 119) / 120), color);
            }
            column += advance;
        }
        if (ed->cursor == end && (end == ed->length || ed->text[end] == '\n')) {
            cursor_x = text_x + (int)column * text_cell;
            cursor_y = y;
            cursor_height = row_height;
            cursor_cell = text_cell;
        }
        if (!tiny_font_set_size(app->font, size))
            return false;
        y += row_height;
        if (next == start || (next == ed->length && (next == 0 || ed->text[next - 1] != '\n')))
            break;
        start = next;
    }
    if (!reading && !ed->command_active && cursor_x >= 0) {
        if (ed->mode == INSERT)
            render_rectangle(pixels, width, height, stride, cursor_x, cursor_y, 2, cursor_height,
                             theme->cursor);
        else
            render_rectangle(pixels, width, height, stride, cursor_x, cursor_y + cursor_height - 3,
                             cursor_cell, 3, theme->cursor);
    }
    render_rectangle(pixels, width, height, stride, 0, status_y, width, status_line + 2 * padding,
                     theme->status);
    char status[512];
    /* Vim modes, the file name and pending keys stay out of the status line; only
     * non-default views are worth calling out. */
    const char *mode = reading ? "READING" : ed->view == VIEW_SOURCE ? "SOURCE" : "";
    int baseline = status_y + padding + ascender;
    if (ed->command_active) {
        char prompt[1100];
        snprintf(prompt, sizeof(prompt), "%c%s_", ed->command_kind ? ed->command_kind : ':',
                 ed->command);
        if (!render_label(app, pixels, width, height, stride, prompt, padding, baseline, cell,
                          theme->status_text))
            return false;
    } else {
        char position[64];
        snprintf(position, sizeof(position), "%zu:%zu", editor_line_number(ed, ed->cursor) + 1,
                 editor_column(ed, ed->cursor) + 1);
        int position_x = width - padding - (int)strlen(position) * cell;
        snprintf(status, sizeof(status), "%s", mode);
        if (app->toast[0] && !ed->message[0]) {
            size_t n = strlen(status);
            snprintf(status + n, sizeof(status) - n, "%s%s", n ? "  " : "", app->toast);
        }
        if (app->work_at && !ed->message[0]) {
            size_t n = strlen(status);
            snprintf(status + n, sizeof(status) - n, "%sindexing image", n ? "  " : "");
        }
        if (!render_label(app, pixels, position_x - cell, height, stride,
                          ed->message[0] ? ed->message : status, padding, baseline, cell,
                          theme->status_text) ||
            !render_label(app, pixels, width, height, stride, position, position_x, baseline, cell,
                          theme->status_text))
            return false;
    }
    if (app->context_menu) {
        static const char *const items[CONTEXT_ITEMS] = {" Copy Image", " Copy Text from Image",
                                                         " Open Image"};
        render_rectangle(pixels, width, height, stride, app->context_x, app->context_y, 26 * cell,
                         CONTEXT_ITEMS * line_height, theme->menu);
        render_rectangle(pixels, width, height, stride, app->context_x,
                         app->context_y + app->context_choice * line_height, 26 * cell, line_height,
                         theme->menu_selected);
        for (int i = 0; i < CONTEXT_ITEMS; ++i)
            if (!render_label(app, pixels, width, height, stride, items[i], app->context_x,
                              app->context_y + i * line_height + ascender, cell,
                              app->context_choice == i ? theme->menu_selected_text
                                                       : theme->menu_text))
                return false;
    }
    if (app->page_map &&
        !navigator_draw(app, theme, pixels, width, height, stride, top, status_y, padding))
        return false;
    if (app->help && !help_draw(app, theme, pixels, width, height, stride, top, status_y, padding))
        return false;
    return true;
}
size_t app_hit_test(struct app *app, int logical_x, int logical_y)
{
    if (app->cell < 1 || app->line_height < 1)
        return app->editor.cursor;
    int x = (int)((int64_t)logical_x * app->scale120 / 120) - app->text_x;
    int y = (int)((int64_t)logical_y * app->scale120 / 120) - app->padding - app->title_height;
    unsigned row = y > 0 ? (unsigned)y : 0;
    if (app->text_height > 0 && row >= (unsigned)app->text_height)
        row = (unsigned)app->text_height - 1;
    struct editor *ed = &app->editor;
    size_t p = ed->top;
    for (;;) {
        unsigned span = (unsigned)app_view_height(app, p);
        if (span && row < span) {
            if (view_image(app, p))
                return p;
            struct md_cache *cache = markdown_at(app, p);
            unsigned content = table_width(app, cache);
            if (content)
                return table_hit(app, cache, x, row / app->line_height, content);
            break;
        }
        size_t next = app_view_next(app, p);
        if (next <= p)
            break;
        row -= span;
        p = next;
    }
    size_t end = app_view_next(app, p);
    struct md_cache *cache = ed->view == VIEW_SOURCE ? NULL : markdown_at(app, p);
    struct text_layout layout = text_layout(app, cache);
    bool reading = ed->view == VIEW_READING;
    unsigned col = reading ? view_indent(app, cache, p) : 0;
    unsigned target = x > 0 ? (unsigned)(x / layout.cell) : 0;
    while (p < end && ed->text[p] != '\n') {
        if (reading && (markdown_style(cache, p) & MD_HIDE)) {
            p = editor_next(ed, p);
            continue;
        }
        unsigned step = ed->text[p] == '\t' ? 4 - col % 4 : 1;
        if (col + step > target)
            break;
        col += step;
        p = editor_next(ed, p);
    }
    return p;
}
int app_corner_radius(const struct app *app, int width, int height, uint32_t scale120)
{
    if (!app->client_decorated || app->maximized || app->tiled || !scale120 ||
        (uint64_t)width * 120 < (uint64_t)scale120 * 2 ||
        (uint64_t)height * 120 < (uint64_t)scale120 * 2)
        return 0;
    int radius = (int)((UINT64_C(12) * scale120 + 60) / 120);
    if (radius > width / 2)
        radius = width / 2;
    if (radius > height / 2)
        radius = height / 2;
    return radius;
}
static uint32_t corner_pixel(uint32_t pixel, unsigned coverage)
{
    uint32_t result = 0;
    for (unsigned shift = 0; shift < 32; shift += 8)
        result |= ((((pixel >> shift) & 255) * coverage + 8) / 16) << shift;
    return result;
}
static void window_corners(struct app *app, uint32_t *pixels, int width, int height, int stride)
{
    int radius = app_corner_radius(app, width, height, app->scale120);
    /* Supersample only the corner squares, with no extra buffer or retained cache.
     * All four corners share the coverage; output is premultiplied for Wayland. */
    int64_t rr = (int64_t)radius * radius * 64;
    for (int y = 0; y < radius; ++y)
        for (int x = 0; x < radius; ++x) {
            unsigned coverage = 0;
            for (int sy = 1; sy < 8; sy += 2)
                for (int sx = 1; sx < 8; sx += 2) {
                    int64_t dx = (int64_t)(radius - x) * 8 - sx;
                    int64_t dy = (int64_t)(radius - y) * 8 - sy;
                    coverage += dx * dx + dy * dy <= rr;
                }
            if (coverage == 16)
                continue;
            size_t offsets[] = {(size_t)y * stride + x, (size_t)y * stride + width - 1 - x,
                                (size_t)(height - 1 - y) * stride + x,
                                (size_t)(height - 1 - y) * stride + width - 1 - x};
            for (unsigned i = 0; i < 4; ++i)
                pixels[offsets[i]] = corner_pixel(pixels[offsets[i]], coverage);
        }
}
bool app_paint(struct app *app, uint32_t *pixels, int width, int height, int stride)
{
    if (!pixels || width < 1 || height < 1 || stride < width)
        return false;
    if (!app->font)
        app->font = tiny_font_open();
    if (!app->font)
        return false;
    app->image_box = false;
    bool ok = paint(app, pixels, width, height, stride);
    if (ok)
        window_corners(app, pixels, width, height, stride);
    tiny_font_end_frame(app->font);
    return ok;
}
