#include "app.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
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
    for (int i = 0; i < STRIDE * HEIGHT; ++i) padded[i] = 0x12345678;
    app.scale120 = 180;
    assert(app_paint(&app, packed, WIDTH, HEIGHT, WIDTH));
    assert(app_paint(&app, padded, WIDTH, HEIGHT, STRIDE));
    for (int y = 0; y < HEIGHT; ++y) {
        assert(!memcmp(packed + y * WIDTH, padded + y * STRIDE, WIDTH * sizeof(*packed)));
        for (int x = WIDTH; x < STRIDE; ++x) assert(padded[y * STRIDE + x] == 0x12345678);
    }
    assert(!app_paint(&app, packed, WIDTH, HEIGHT, WIDTH - 1));
    free(packed); free(padded);
    app_input(&app, KEY_TEXT, ":");
    app_input(&app, KEY_TEXT, "q!");
    app_input(&app, KEY_ENTER, "");
    assert(!app.running);
    app_close(&app);
    assert(!app.font && !app.editor.text);
    puts("Shared input, close protection, and pitched rendering passed");
}
