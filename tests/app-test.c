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
    char lines[101];
    memset(lines, '\n', sizeof(lines));
    const size_t counts[] = {9, 10, 99, 100};
    for (size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); ++i) {
        assert(editor_replace(&app.editor, 0, app.editor.length, lines, counts[i] - 1));
        app.editor.cursor = 0;
        assert(app_paint(&app, pixels, 320, 200, 320));
        assert(app.gutter == 6 * app.cell);
        assert(app_hit_test(&app, app.padding + app.gutter, app.padding) == 0);
    }
    app.editor.line_numbers = false;
    assert(app_paint(&app, pixels, 320, 200, 320));
    assert(app.gutter == 0);
    app_close(&app);
}

int main(void)
{
    gutter_layout();
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
    app_input(&app, KEY_TEXT, "set nu");
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
