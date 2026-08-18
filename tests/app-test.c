#include "app.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void gutter_layout(void)
{
    struct app app;
    assert(app_open(&app, NULL));
    uint32_t pixels[320 * 200];
    char *lines = malloc(100000);
    assert(lines);
    memset(lines, '\n', 100000);
    const struct { size_t lines; int digits; } layouts[] = {
        {9, 4}, {10, 4}, {99, 4}, {100, 4}, {999, 4}, {1000, 4},
        {9999, 4}, {10000, 5}, {99999, 5}, {100000, 6},
    };
    for (size_t i = 0; i < sizeof(layouts) / sizeof(layouts[0]); ++i) {
        assert(editor_replace(&app.editor, 0, app.editor.length, lines, layouts[i].lines - 1));
        app.editor.cursor = 0;
        assert(app_paint(&app, pixels, 320, 200, 320));
        assert(app.gutter == (layouts[i].digits + 2) * app.cell);
        assert(app_hit_test(&app, app.padding + app.gutter, app.padding) == 0);
        bool number_ink = false;
        int number_x = app.padding + (layouts[i].digits - 1) * app.cell;
        int gap_x = number_x + app.cell;
        for (int y = app.padding; y < app.padding + app.line_height; ++y) {
            for (int x = number_x; x < gap_x; ++x)
                number_ink |= pixels[y * 320 + x] != 0xff18202b;
            for (int x = gap_x; x < app.padding + app.gutter; ++x)
                assert(pixels[y * 320 + x] == 0xff18202b);
        }
        assert(number_ink); /* right-aligned number, then two clear text cells */
    }
    free(lines);
    assert(editor_replace(&app.editor, 0, app.editor.length, "\n", 1));
    app.editor.cursor = 0;
    editor_command(&app.editor, "set nonu");
    assert(strstr(app.editor.message, "Unknown command"));
    assert(app_paint(&app, pixels, 320, 200, 320));
    assert(app.gutter == 6 * app.cell);
    assert(editor_replace(&app.editor, 0, app.editor.length, "one two one", 11));
    app.editor.cursor = 0;
    app_input(&app, KEY_TEXT, "/");
    app_input(&app, KEY_TEXT, "one");
    app_input(&app, KEY_ENTER, "");
    editor_command(&app.editor, "noh");
    assert(strstr(app.editor.message, "Unknown command"));
    assert(app_paint(&app, pixels, 320, 200, 320));
    int text_x = app.padding + app.gutter;
    assert(pixels[app.padding * 320 + text_x] == 0xff66502c);
    assert(pixels[app.padding * 320 + text_x + 8 * app.cell] == 0xff66502c);
    app_close(&app);
}

static void theme_layout(void)
{
    struct app app;
    assert(app_open(&app, NULL));
    assert(!app.editor.light);
    const char *note = "`code` match\n# Heading\n> Quote\n";
    assert(editor_replace(&app.editor, 0, 0, note, strlen(note)));
    app.editor.cursor = 0;
    strcpy(app.editor.search, "match");
    enum { WIDTH = 320, HEIGHT = 200, STRIDE = 327 };
    uint32_t *dark = malloc(WIDTH * HEIGHT * sizeof(*dark));
    uint32_t *light = malloc(STRIDE * HEIGHT * sizeof(*light));
    assert(dark && light);
    assert(app_paint(&app, dark, WIDTH, HEIGHT, WIDTH));
    unsigned columns = app.editor.viewport_columns, rows = app.editor.viewport_rows;
    uint64_t revision = app.editor.revision;
    app_input(&app, KEY_TEXT, ":");
    app_input(&app, KEY_TEXT, "set light");
    app_input(&app, KEY_ENTER, "");
    assert(app.editor.light && app.editor.revision == revision);
    for (int i = 0; i < STRIDE * HEIGHT; ++i)
        light[i] = 0x12345678;
    assert(app_paint(&app, light, WIDTH, HEIGHT, STRIDE));
    assert(app.editor.viewport_columns == columns && app.editor.viewport_rows == rows);
    assert(light[0] == 0xfffaf9f6 && light[(HEIGHT - 1) * STRIDE] == 0xffe5e8eb);
    int x = app.padding + app.gutter, y = app.padding;
    assert(light[y * STRIDE + x + app.cell] == 0xffeeede7); /* inline code */
    assert(light[y * STRIDE + x + 7 * app.cell] == 0xfff5dc8e); /* search */
    for (int row = 0; row < HEIGHT; ++row)
        for (int col = WIDTH; col < STRIDE; ++col)
            assert(light[row * STRIDE + col] == 0x12345678);
    app.editor.mode = VISUAL;
    app.editor.anchor = 0;
    assert(app_paint(&app, light, WIDTH, HEIGHT, STRIDE));
    assert(light[y * STRIDE + x] == 0xffc5dced);
    app.editor.mode = NORMAL;
    app.context_menu = true;
    app.context_x = x;
    app.context_y = y + app.line_height;
    assert(app_paint(&app, light, WIDTH, HEIGHT, STRIDE));
    assert(light[app.context_y * STRIDE + x] == 0xffcbdbe8);
    assert(light[(app.context_y + app.line_height) * STRIDE + x] == 0xffe9edf1);
    app.context_menu = false;
    editor_command(&app.editor, "set dark");
    assert(!app.editor.light);
    assert(app_paint(&app, light, WIDTH, HEIGHT, STRIDE));
    for (int row = 0; row < HEIGHT; ++row)
        assert(!memcmp(dark + row * WIDTH, light + row * STRIDE, WIDTH * sizeof(*dark)));
    assert(!strcmp(app.editor.text, note) && app.editor.revision == revision);
    free(dark);
    free(light);
    app_close(&app);
}

static void decoration_layout(void)
{
    struct app app;
    assert(app_open(&app, NULL));
    assert(editor_replace(&app.editor, 0, 0, "first\nsecond\n", 13));
    app.editor.cursor = 0;
    enum { WIDTH = 640, HEIGHT = 400 };
    uint32_t *pixels = malloc(WIDTH * HEIGHT * sizeof(*pixels));
    assert(pixels);
    const unsigned scales[] = {120, 150, 180, 240};
    for (unsigned i = 0; i < sizeof(scales) / sizeof(scales[0]); ++i) {
        app.scale120 = scales[i];
        app.editor.light = i % 2 != 0;
        app.client_decorated = false;
        assert(app_paint(&app, pixels, WIDTH, HEIGHT, WIDTH));
        unsigned rows = app.editor.viewport_rows;
        app.client_decorated = true;
        assert(app_paint(&app, pixels, WIDTH, HEIGHT, WIDTH));
        int h = app.title_height, y = h / 2 * 120 / (int)scales[i];
        assert(h == app.line_height + app.padding && app.editor.viewport_rows < rows);
        assert(pixels[0] == (app.editor.light ? 0xfff2f1ee : 0xff1d2733));
        assert(app_title_hit(&app, 20, y) == TITLE_MOVE);
        assert(app_title_hit(&app, (WIDTH - 5 * h / 2) * 120 / (int)scales[i], y) == TITLE_MINIMIZE);
        assert(app_title_hit(&app, (WIDTH - 3 * h / 2) * 120 / (int)scales[i], y) == TITLE_MAXIMIZE);
        assert(app_title_hit(&app, (WIDTH - h / 2) * 120 / (int)scales[i], y) == TITLE_CLOSE);
        for (enum app_title_action action = TITLE_MINIMIZE; action <= TITLE_CLOSE; ++action) {
            int stroke = (int)(scales[i] / 120);
            int sample = WIDTH - (TITLE_CLOSE - action + 1) * h + 4 * stroke;
            app.title_hover = action;
            assert(app_paint(&app, pixels, WIDTH, HEIGHT, WIDTH));
            uint32_t hover = action == TITLE_CLOSE ? 0xffc74242
                             : app.editor.light ? 0xffe0e2df : 0xff303e4e;
            assert(pixels[h / 2 * WIDTH + sample] == hover);
            app.title_pressed = action;
            if (action == TITLE_MAXIMIZE)
                app.maximized = true; /* the restore button shares the same feedback */
            assert(app_paint(&app, pixels, WIDTH, HEIGHT, WIDTH));
            uint32_t pressed = action == TITLE_CLOSE ? 0xffa83333
                               : app.editor.light ? 0xffcdd3d0 : 0xff3d5064;
            assert(pixels[h / 2 * WIDTH + sample] == pressed);
            app.title_hover = TITLE_NONE;
            assert(app_paint(&app, pixels, WIDTH, HEIGHT, WIDTH));
            assert(pixels[h / 2 * WIDTH + sample] == pixels[0]);
            app.title_pressed = TITLE_NONE;
            app.maximized = false;
        }
        int x = (app.padding + app.gutter) * 120 / (int)scales[i];
        int first = (h + app.padding + app.line_height / 2) * 120 / (int)scales[i];
        int second = first + app.line_height * 120 / (int)scales[i];
        assert(app_hit_test(&app, x, first) == 0);
        assert(app_hit_test(&app, x, second) == 6);
        assert(app_title_hit(&app, x, second) == TITLE_NONE);
        app.client_decorated = false;
        assert(app_paint(&app, pixels, WIDTH, HEIGHT, WIDTH));
        assert(app.title_height == 0 && app.editor.viewport_rows == rows);
        assert(app_title_hit(&app, 20, y) == TITLE_NONE);
    }
    free(pixels);
    app_close(&app);
}

static void render_modes(void)
{
    struct app app;
    assert(app_open(&app, NULL));
    const char *note = "# Head\n**bold** [label](https://example.com/a/very/long/destination) tail\n"
                       "> quote\n- [x] task\n```\n# raw\n```\n";
    assert(editor_replace(&app.editor, 0, 0, note, strlen(note)));
    app.editor.cursor = 0;
    enum { WIDTH = 320, HEIGHT = 400 };
    uint32_t *pixels = malloc(WIDTH * HEIGHT * sizeof(*pixels));
    assert(pixels);
    assert(app.editor.render_mode == RENDER_NORMAL);
    assert(app_paint(&app, pixels, WIDTH, HEIGHT, WIDTH));
    int normal_height = app.line_height;
    size_t second = (size_t)(strstr(note, "**bold") - note);
    assert(app_view_next(&app, second) < editor_line_end(&app.editor, second));
    editor_command(&app.editor, "set raw");
    assert(app_paint(&app, pixels, WIDTH, HEIGHT, WIDTH));
    assert(app.gutter == 6 * app.cell && app.line_height == normal_height);
    assert(app_hit_test(&app, app.text_x, app.padding + app.line_height / 2) == 0);
    uint64_t revision = app.editor.revision;
    editor_command(&app.editor, "set polish");
    assert(app_paint(&app, pixels, WIDTH, HEIGHT, WIDTH));
    assert(app.gutter == 0 && app.text_x > app.padding && app.line_height > normal_height);
    assert(app_view_next(&app, second) == editor_line_end(&app.editor, second) + 1);
    int x = app.text_x, first = app.padding + app.line_height / 2;
    assert(app_hit_test(&app, x, first) == 2); /* heading marker is concealed */
    int body_y = app.padding + app_view_height(&app, 0);
    int body_first = body_y + app.line_height / 2;
    assert(app_hit_test(&app, x, body_first) == second + 2);
    assert(app_hit_test(&app, x + 5 * app.cell, body_first) ==
           (size_t)(strstr(note, "label") - note));
    assert(app_hit_test(&app, x + 11 * app.cell, body_first) ==
           (size_t)(strstr(note, "tail") - note));
    assert(app_hit_test(&app, x, body_first + app.line_height) ==
           (size_t)(strstr(note, "quote") - note));
    assert(app_hit_test(&app, x + 2 * app.cell, body_first + 2 * app.line_height) ==
           (size_t)(strstr(note, "task") - note));
    size_t fence = (size_t)(strstr(note, "```") - note);
    assert(app_view_rows(&app, fence) == 0);
    assert(app_hit_test(&app, x, body_first + 3 * app.line_height) ==
           (size_t)(strstr(note, "# raw") - note));
    assert(pixels[(body_y + 3 * app.line_height) * WIDTH + x] == 0xff242d3b);
    assert(pixels[(body_y + app.line_height) * WIDTH + x - app.padding] == 0xffa3be8c);
    app_pointer(&app, x, body_first, false);
    app_pointer(&app, x + 3 * app.cell, body_first, true);
    assert(app.editor.mode == VISUAL && app.editor.anchor == second + 2 &&
           app.editor.cursor == second + 5);
    assert(app.editor.revision == revision && !strcmp(app.editor.text, note));
    app_input(&app, KEY_TEXT, "d");
    assert(app.editor.revision == revision && !strcmp(app.editor.text, note));
    editor_command(&app.editor, "set normal");
    assert(app_paint(&app, pixels, WIDTH, HEIGHT, WIDTH));
    assert(app.gutter == 6 * app.cell && app.line_height == normal_height);
    assert(!strcmp(app.editor.text, note));
    free(pixels);
    app_close(&app);
}

static unsigned style_at(const char *text, const char *needle)
{
    struct md_span spans[128];
    size_t n = markdown_line(text, 0, strlen(text), spans, 128, false);
    size_t at = (size_t)(strstr(text, needle) - text);
    unsigned style = 0;
    for (size_t i = 0; i < n; ++i)
        if (at >= spans[i].start && at < spans[i].end) style |= spans[i].style;
    return style;
}
static void markdown_notes(void)
{
    assert(style_at("**bold and *nested italic***", "nested") == (MD_BOLD | MD_ITALIC));
    assert(style_at("*italic and **nested bold***", "nested") == (MD_BOLD | MD_ITALIC));
    assert(style_at("***both***", "both") == (MD_BOLD | MD_ITALIC));
    assert(style_at("~~old **bold**~~", "bold") == (MD_STRIKE | MD_BOLD));
    assert(style_at("`~~literal~~`", "literal") == MD_CODE);
    assert(style_at("https://example.org/a_b", "a_b") == MD_LINK);
    assert(!(style_at("word_with_underscores", "with") & MD_ITALIC));
    assert(markdown_block("  12) item", 0, 10).indent == 6);
    assert(markdown_block("    - [x] task", 0, 14).indent == 6);
    assert(markdown_block("* * *", 0, 5).rule);
    assert(!markdown_block("- text", 0, 6).rule);
    assert(!markdown_block("1234567890. text", 0, 16).indent);
    struct app app;
    assert(app_open(&app, NULL));
    const char *note = "## Heading\n  12. a long nested list item with several words for wrapping\n---\n";
    assert(editor_replace(&app.editor, 0, 0, note, strlen(note)));
    editor_command(&app.editor, "set polish");
    uint32_t pixels[400 * 400];
    assert(app_paint(&app, pixels, 400, 400, 400));
    assert(app_view_rows(&app, 0) == 2);
    size_t list = (size_t)(strstr(note, "  12.") - note);
    size_t wrap = app_view_next(&app, list);
    assert(wrap > list && wrap < editor_line_end(&app.editor, list));
    /* Continuation text begins under the first item character, not the number. */
    assert(app_hit_test(&app, app.text_x + 6 * app.cell,
                       app.padding + app_view_height(&app, 0) + app.line_height) == wrap);
    size_t rule = (size_t)(strstr(note, "---") - note);
    assert(app_view_next(&app, rule) == strlen(note));
    editor_command(&app.editor, "set raw");
    assert(app_paint(&app, pixels, 400, 400, 400));
    assert(app_view_rows(&app, 0) == 1);
    assert(!strcmp(app.editor.text, note));
    app_close(&app);
}

static void markdown_tables(void)
{
    const char *note = "| Name | Value |\n| :--- | ---: |\n"
                       "| **alpha** with several words that wrap neatly | 42 |\n"
                       "| literal \\| pipe | `code` |\n";
    struct md_table table = {0};
    size_t separator = (size_t)(strchr(note, '\n') - note) + 1;
    size_t body = (size_t)(strchr(note + separator, '\n') - note) + 1;
    size_t last = (size_t)(strchr(note + body, '\n') - note) + 1;
    markdown_table(note, strlen(note), 0, separator - 1, &table, false);
    assert(table.count == 2 && table.header && !table.separator);
    assert(table.align[0] == 0 && table.align[1] == 2);
    markdown_table(note, strlen(note), separator, body - 1, &table, true);
    assert(table.separator && !table.header);
    markdown_table(note, strlen(note), body, last - 1, &table, true);
    assert(table.count == 2 && !table.header && !table.separator);
    memset(&table, 0, sizeof(table));
    markdown_table(note, strlen(note), last, strlen(note) - 1, &table, false);
    assert(table.count == 2 && table.align[1] == 2); /* direct access into body */
    assert(table.cells[0].end - table.cells[0].start == strlen("literal \\| pipe"));
    const char *bad = "a | b\n-- | ---\n";
    markdown_table(bad, strlen(bad), 0, 5, &table, false);
    assert(!table.count);
    struct md_cell cells[MD_TABLE_COLUMNS];
    const char *wide = "a|b|c|d|e|f|g|h|i|j|k|l|m|n|o|p|q";
    assert(!markdown_cells(wide, 0, strlen(wide), cells));
    struct app app;
    assert(app_open(&app, NULL));
    assert(editor_replace(&app.editor, 0, 0, note, strlen(note)));
    editor_command(&app.editor, "set polish");
    uint32_t pixels[400 * 400];
    assert(app_paint(&app, pixels, 400, 400, 400));
    assert(app_view_rows(&app, separator) == 0);
    assert(app_view_next(&app, body) == last);
    int body_rows = app_view_rows(&app, body);
    assert(body_rows > 1);
    /* Clicking wrapped/hidden source and a right-aligned number. */
    assert(app_hit_test(&app, app.text_x + app.cell,
                       app.padding + app.line_height) == body + 4);
    assert(app_hit_test(&app, app.text_x + app.cell,
                       app.padding + 2 * app.line_height) ==
           (size_t)(strstr(note, "several") - note));
    unsigned content = (app.editor.viewport_columns - 1) / 2 - 3;
    int number_x = app.text_x + (int)(2 * content + 2) * app.cell;
    assert(app_hit_test(&app, number_x, app.padding + app.line_height) ==
           (size_t)(strstr(note, "42") - note));
    assert(app_hit_test(&app, app.text_x + app.cell,
                       app.padding + (body_rows + 1) * app.line_height) == last + 2);
    /* A resize must retain access to long rows through the source fallback. */
    assert(app_paint(&app, pixels, 120, 200, 120));
    assert(app_view_next(&app, body) < last);
    app.editor.cursor = app.editor.top = 0;
    editor_command(&app.editor, "set raw");
    assert(app_paint(&app, pixels, 400, 400, 400));
    assert(app_view_rows(&app, separator) == 1);
    assert(app_hit_test(&app, app.text_x, app.padding) == 0);
    assert(!strcmp(app.editor.text, note));
    const char *code = "```\n| a | b |\n| --- | --- |\n```\n";
    assert(editor_replace(&app.editor, 0, app.editor.length, code, strlen(code)));
    app.editor.cursor = app.editor.top = 0;
    editor_command(&app.editor, "set polish");
    assert(app_paint(&app, pixels, 400, 400, 400));
    size_t code_separator = (size_t)(strstr(code, "| ---") - code);
    assert(app_view_rows(&app, code_separator) == 1 && !app.markdown.table.count);
    app_close(&app);
}

static void normal_markdown_styles(void)
{
    assert(style_at("***bolditalic***", "bolditalic") == (MD_BOLD | MD_ITALIC));
    assert(style_at("~~strikethrough~~", "strikethrough") == MD_STRIKE);
    const char *note = "***slanted***\n **slanted**\n~~xxxx~~\n```text\nshort\n\n```\nafter\n";
    const unsigned scales[] = {120, 180, 240};
    for (unsigned light = 0; light < 2; ++light) {
        for (unsigned scale = 0; scale < 3; ++scale) {
            struct app app;
            assert(app_open(&app, NULL));
            assert(editor_replace(&app.editor, 0, 0, note, strlen(note)));
            app.editor.cursor = app.editor.length - 1;
            app.editor.light = light;
            app.scale120 = scales[scale];
            int width = 400 * (int)app.scale120 / 120;
            int height = 400 * (int)app.scale120 / 120;
            uint32_t *pixels = malloc((size_t)width * height * sizeof(*pixels));
            assert(pixels && app_paint(&app, pixels, width, height, width));
            uint32_t background = light ? 0xfffaf9f6 : 0xff18202b;
            uint32_t code = light ? 0xffeeede7 : 0xff242d3b;
            uint32_t text = light ? 0xff263342 : 0xffe5edf5;
            /* Both rows have text in the same columns; italic changes the shape. */
            bool slanted = false;
            for (int y = 0; y < app.line_height; ++y)
                for (int x = app.text_x + 3 * app.cell; x < app.text_x + 10 * app.cell; ++x)
                    slanted |= (pixels[(app.padding + y) * width + x] != background) !=
                               (pixels[(app.padding + app.line_height + y) * width + x] != background);
            assert(slanted);
            int strike_y = app.padding + 2 * app.line_height + tiny_font_metrics(app.font).ascender * 3 / 4;
            for (int x = app.text_x + 2 * app.cell; x < app.text_x + 6 * app.cell; ++x)
                assert(pixels[strike_y * width + x] == text);
            /* Fence markers, content and empty lines all shade through the right edge. */
            for (int row = 3; row <= 6; ++row) {
                int y = app.padding + row * app.line_height;
                assert(pixels[y * width + app.text_x + app.text_width - 1] == code);
                assert(pixels[y * width + app.padding] == background); /* gutter stays clear */
            }
            assert(pixels[(app.padding + 5 * app.line_height) * width + app.text_x] == code);
            editor_command(&app.editor, "set raw");
            assert(app_paint(&app, pixels, width, height, width));
            assert(pixels[(app.padding + 5 * app.line_height) * width + app.text_x] == background);
            free(pixels);
            app_close(&app);
        }
    }
}

static void heading_layout(void)
{
    const char *note = "# Heading\n## Heading\n### Heading\n#### Heading\n##### Heading\n###### Heading\n"
                       "body\n# A much longer heading with enough words to wrap across several lines\ntail\n";
    const unsigned scales[] = {120, 180, 240};
    for (unsigned light = 0; light < 2; ++light) {
        for (unsigned scale = 0; scale < 3; ++scale) {
            struct app app;
            assert(app_open(&app, NULL));
            assert(editor_replace(&app.editor, 0, 0, note, strlen(note)));
            app.scale120 = scales[scale];
            app.editor.light = light;
            int width = 360 * (int)app.scale120 / 120;
            int height = 720 * (int)app.scale120 / 120;
            uint32_t *pixels = malloc((size_t)width * height * sizeof(*pixels));
            assert(pixels);
            int normal_h1 = 0;
            const char *modes[] = {"set normal", "set polish", "set raw"};
            for (unsigned mode = 0; mode < 3; ++mode) {
                editor_command(&app.editor, modes[mode]);
                app.editor.cursor = app.editor.top = 0;
                app.follow_cursor = false;
                assert(app_paint(&app, pixels, width, height, width));
                size_t p = 0;
                int y = app.padding, first_height = app_view_height(&app, 0), previous = first_height;
                if (mode == 0) normal_h1 = first_height;
                if (mode == 1) assert(first_height > normal_h1);
                for (unsigned level = 1; level <= 6; ++level) {
                    int h = app_view_height(&app, p);
                    assert(h <= previous);
                    if (mode == 2) assert(h == app.line_height);
                    /* Every heading's left edge maps to its own source line. */
                    int logical_x = (app.text_x + 1) * 120 / (int)app.scale120;
                    int logical_y = (y + h / 2) * 120 / (int)app.scale120;
                    assert(app_hit_test(&app, logical_x, logical_y) == p + (mode == 1 ? level + 1 : 0));
                    previous = h;
                    y += h;
                    p = editor_line_end(&app.editor, p) + 1;
                }
                unsigned base_size = 18 * 64 * app.scale120 / 120;
                unsigned heading_size = mode == 0 ? (base_size * 118 + 50) / 100
                                       : mode == 1 ? (base_size * 190 + 50) / 100 : base_size;
                int heading_cell = tiny_font_metrics_at(heading_size).advance;
                unsigned target_column = mode == 1 ? 3 : 6;
                int logical_click_x = (app.text_x + (int)target_column * heading_cell + heading_cell / 2)
                                      * 120 / (int)app.scale120;
                int logical_click_y = (app.padding + first_height / 2) * 120 / (int)app.scale120;
                assert(app_hit_test(&app, logical_click_x, logical_click_y) ==
                       target_column + (mode == 1 ? 2 : 0));
                if (mode == 0) {
                    app.editor.cursor = 6;
                    assert(app_paint(&app, pixels, width, height, width));
                    int cursor_x = app.text_x + 6 * heading_cell + heading_cell / 2;
                    int cursor_y = app.padding + first_height - 2;
                    assert(pixels[cursor_y * width + cursor_x] == (light ? 0xff1d6972 : 0xff8fbcbb));
                    app.editor.cursor = 0;
                }
                if (mode != 2) assert(first_height > previous);
                assert(app_view_height(&app, p) == app.line_height); /* body size restored */
                p = editor_line_end(&app.editor, p) + 1;
                y += app.line_height;
                size_t wrap = app_view_next(&app, p);
                assert(wrap > p && wrap < editor_line_end(&app.editor, p));
                int h = app_view_height(&app, p);
                int logical_x = (app.text_x + 1) * 120 / (int)app.scale120;
                int logical_y = (y + h + h / 2) * 120 / (int)app.scale120;
                assert(app_hit_test(&app, logical_x, logical_y) == wrap);
                /* On a shorter window, follow the cursor using actual heights. */
                app.editor.cursor = (size_t)(strstr(note, "tail") - note);
                app.follow_cursor = true;
                int short_height = 180 * (int)app.scale120 / 120;
                assert(app_paint(&app, pixels, width, short_height, width));
                assert(app.editor.top > 0);
                int cursor_y = 0;
                p = app.editor.top;
                while (p < app.editor.cursor) {
                    cursor_y += app_view_height(&app, p);
                    size_t next = app_view_next(&app, p);
                    assert(next > p && next <= app.editor.cursor);
                    p = next;
                }
                assert(cursor_y + app_view_height(&app, p) <= app.text_height);
            }
            assert(!strcmp(app.editor.text, note));
            free(pixels);
            app_close(&app);
        }
    }
}

int main(void)
{
    gutter_layout();
    theme_layout();
    decoration_layout();
    render_modes();
    markdown_notes();
    markdown_tables();
    normal_markdown_styles();
    heading_layout();
    struct app app;
    assert(app_open(&app, NULL));
    app_input(&app, KEY_TEXT, "i");
    app_input(&app, KEY_TEXT, "Shared editor\n");
    app_input(&app, KEY_ESCAPE, "");
    assert(app.editor.modified && app.running);
    app_request_close(&app);
    assert(app.running && strstr(app.editor.message, "Unsaved"));
    enum { WIDTH = 320, HEIGHT = 200, STRIDE = 337 };
    uint32_t *packed = malloc(WIDTH * HEIGHT * sizeof(*packed));
    uint32_t *padded = malloc(STRIDE * HEIGHT * sizeof(*padded));
    assert(packed && padded);
    for (int i = 0; i < STRIDE * HEIGHT; ++i)
        padded[i] = 0x12345678;
    app.scale120 = 180;
    assert(app_paint(&app, packed, WIDTH, HEIGHT, WIDTH));
    assert(app_paint(&app, padded, WIDTH, HEIGHT, STRIDE));
    for (int y = 0; y < HEIGHT; ++y) {
        assert(!memcmp(packed + y * WIDTH, padded + y * STRIDE, WIDTH * sizeof(*packed)));
        for (int x = WIDTH; x < STRIDE; ++x)
            assert(padded[y * STRIDE + x] == 0x12345678);
    }
    /* Commands replace the status row without changing the text viewport. */
    app.editor.message[0] = 0;
    assert(app_paint(&app, packed, WIDTH, HEIGHT, WIDTH));
    int status_y = HEIGHT - app.line_height - 2 * app.padding;
    assert(app.editor.viewport_rows == (unsigned)((status_y - app.padding) / app.line_height));
    app_input(&app, KEY_TEXT, ":");
    app_input(&app, KEY_TEXT, "set md");
    assert(app_paint(&app, padded, WIDTH, HEIGHT, STRIDE));
    assert(app.editor.viewport_rows == (unsigned)((status_y - app.padding) / app.line_height));
    bool status_changed = false;
    for (int y = status_y; y < HEIGHT; ++y)
        status_changed |= memcmp(packed + y * WIDTH, padded + y * STRIDE,
                                 WIDTH * sizeof(*packed)) != 0;
    assert(status_changed);
    app_input(&app, KEY_ESCAPE, "");
    assert(app_paint(&app, padded, WIDTH, HEIGHT, STRIDE));
    for (int y = 0; y < HEIGHT; ++y)
        assert(!memcmp(packed + y * WIDTH, padded + y * STRIDE, WIDTH * sizeof(*packed)));
    assert(!app_paint(&app, packed, WIDTH, HEIGHT, WIDTH - 1));
    free(packed);
    free(padded);
    /* A visual put uses the clipboard path and replaces, as one undo step. */
    editor_change_end(&app.editor);
    assert(editor_replace(&app.editor, 0, app.editor.length, "one two", 7));
    app.editor.cursor = 0;
    app.editor.mode = NORMAL;
    const char *commands = "yiwwvep";
    while (*commands) {
        char key[2] = {*commands++, 0};
        app_input(&app, KEY_TEXT, key);
    }
    assert(!strcmp(app.editor.text, "one one"));
    app_input(&app, KEY_TEXT, "u");
    assert(!strcmp(app.editor.text, "one two"));
    app_input(&app, KEY_TEXT, ":");
    app_input(&app, KEY_TEXT, "q!");
    app_input(&app, KEY_ENTER, "");
    assert(!app.running);
    app_close(&app);
    assert(!app.font && !app.editor.text);
    puts("Shared input, close protection, and pitched rendering passed");
}
