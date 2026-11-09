/* libFuzzer harness for what a notes folder or another app can hand mib: image files, page
 * text as it is drawn in every view, and pasted fragments. The first byte picks the target.
 * Run it with make fuzz. */
#include "app.h"
#include "media.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static struct app app;
static uint32_t pixels[480 * 320];

static void image(const uint8_t *data, size_t size)
{
    int w, h;
    if (!image_info(data, size, &w, &h))
        return;
    struct image decoded = {0};
    if (image_decode(&decoded, data, size)) {
        size_t length;
        free(image_png(decoded.rgba, decoded.width, decoded.height, &length));
        image_free(&decoded);
    }
}

/* Page text has no NUL bytes, as editor_open refuses them. */
static void page(const uint8_t *data, size_t size)
{
    if (memchr(data, 0, size) || size > 1 << 16)
        return;
    static const char *const views[] = {"set view=source", "set view=normal", "set view=reading"};
    for (unsigned v = 0; v < 3; ++v) {
        editor_command(&app.editor, views[v]);
        if (!editor_replace(&app.editor, 0, app.editor.length, (const char *)data, size))
            return;
        app.editor.cursor = size / 2 < app.editor.length ? size / 2 : 0;
        app.editor.top = 0;
        app_paint(&app, pixels, 480, 320, 480);
    }
}

/* Without a page folder, a fragment's images are read but never stored. */
static void fragment(const uint8_t *data, size_t size)
{
    editor_command(&app.editor, "set view=normal");
    media_paste_fragment(&app, data, size);
    editor_replace(&app.editor, 0, app.editor.length, "", 0);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static bool ready;
    if (!ready) {
        if (!app_open(&app))
            abort();
        free(app.directory);
        app.directory = NULL;
        ready = true;
    }
    if (!size)
        return 0;
    switch (data[0] % 3) {
    case 0:
        image(data + 1, size - 1);
        break;
    case 1:
        page(data + 1, size - 1);
        break;
    default:
        fragment(data + 1, size - 1);
        break;
    }
    return 0;
}
