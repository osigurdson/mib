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
    assert(app_hit_test(&app, x, first + app.line_height) == second + 2);
    assert(app_hit_test(&app, x + 5 * app.cell, first + app.line_height) ==
           (size_t)(strstr(note, "label") - note));
    assert(app_hit_test(&app, x + 11 * app.cell, first + app.line_height) ==
           (size_t)(strstr(note, "tail") - note));
    assert(app_hit_test(&app, x, first + 2 * app.line_height) ==
           (size_t)(strstr(note, "quote") - note));
    assert(app_hit_test(&app, x + 2 * app.cell, first + 3 * app.line_height) ==
           (size_t)(strstr(note, "task") - note));
    size_t fence = (size_t)(strstr(note, "```") - note);
    assert(app_view_rows(&app, fence) == 0);
    assert(app_hit_test(&app, x, first + 4 * app.line_height) ==
           (size_t)(strstr(note, "# raw") - note));
    assert(pixels[(app.padding + 4 * app.line_height) * WIDTH + x] == 0xff242d3b);
    assert(pixels[(app.padding + 2 * app.line_height) * WIDTH + x - app.padding] == 0xffa3be8c);
    app_pointer(&app, x, first + app.line_height, false);
    app_pointer(&app, x + 3 * app.cell, first + app.line_height, true);
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

int main(void)
{
    gutter_layout();
    theme_layout();
    decoration_layout();
    render_modes();
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
