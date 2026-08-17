#include "../src/platform/linux/wayland.c"
#include <assert.h>

static void press(struct wayland_client *client, xkb_keysym_t symbol)
{
    for (xkb_keycode_t code = xkb_keymap_min_keycode(client->xkb_keymap);
         code <= xkb_keymap_max_keycode(client->xkb_keymap); ++code) {
        for (xkb_level_index_t level = 0; level < 2; ++level) {
            const xkb_keysym_t *symbols;
            int count = xkb_keymap_key_get_syms_by_level(client->xkb_keymap, code, 0, level, &symbols);
            if (count == 1 && symbols[0] == symbol) {
                xkb_mod_index_t shift = xkb_keymap_mod_get_index(client->xkb_keymap, XKB_MOD_NAME_SHIFT);
                xkb_state_update_mask(client->xkb_state, level ? 1u << shift : 0, 0, 0, 0, 0, 0);
                handle_key(client, code - 8);
                return;
            }
        }
    }
    assert(!"Could not map keysym");
}

static void frame_limit(void)
{
    struct app app = {.dirty = true, .redraw_at = 1034};
    struct wayland_client client = {.app = &app, .configured = true};
    assert(redraw_delay(&client, 1000) == APP_FRAME_INTERVAL_MS);
    assert(redraw_delay(&client, 1033) == 1);
    assert(redraw_delay(&client, 1034) == 0);
    assert(redraw_delay(&client, 2000) == 0);
    client.pending_buffers = 2;
    assert(redraw_delay(&client, 2000) == -1); /* wait for a buffer, not a timer */
    client.pending_buffers = 0;
    app.dirty = false;
    assert(redraw_delay(&client, 2000) == -1); /* no idle wakeups */
    app.dirty = true;
    client.configured = false;
    assert(redraw_delay(&client, 2000) == -1);
    client.configured = true;
    app.redraw_at = session_now() + 1000;
    uint64_t deadline = app.redraw_at;
    assert(draw(&client) && app.dirty && app.redraw_at == deadline);
}

int main(int argc, char **argv)
{
    frame_limit();
    struct app app = {.width = 800, .height = 560, .scale120 = 180, .running = true};
    char path[] = "/tmp/jot-integration-XXXXXX";
    int fd = mkstemp(path); assert(fd >= 0); close(fd);
    assert(editor_open(&app.editor, path));
    struct wayland_client client = {.app = &app};
    client.xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS); assert(client.xkb_context);
    struct xkb_rule_names names = {.layout = "us"};
    client.xkb_keymap = xkb_keymap_new_from_names(client.xkb_context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    assert(client.xkb_keymap);
    client.xkb_state = xkb_state_new(client.xkb_keymap); assert(client.xkb_state);
    press(&client, XKB_KEY_i);
    const char *text = "Jot, directly on Wayland.\nCPU rendering with tiny-font.\n\n";
    for (const char *p = text; *p; ++p) press(&client, *p == '\n' ? XKB_KEY_Return : (xkb_keysym_t)*p);
    assert(!strcmp(app.editor.text, text));
    press(&client, XKB_KEY_Escape);
    press(&client, XKB_KEY_k); press(&client, XKB_KEY_k);
    press(&client, XKB_KEY_colon); press(&client, XKB_KEY_w); press(&client, XKB_KEY_Return);
    assert(!app.editor.modified);
    FILE *file = fopen(path, "rb"); assert(file);
    char saved[256] = {0}; fread(saved, 1, sizeof(saved) - 1, file); fclose(file);
    assert(!strcmp(saved, text));
    int width = 1200, height = 840;
    uint32_t *pixels = calloc((size_t)width * height, sizeof(*pixels)); assert(pixels);
    assert(app_paint(&app, pixels, width, height, width));
    if (argc == 2) {
        FILE *preview = fopen(argv[1], "wb"); assert(preview);
        fprintf(preview, "P6\n%d %d\n255\n", width, height);
        for (int i = 0; i < width * height; ++i) {
            unsigned char rgb[] = {pixels[i] >> 16, pixels[i] >> 8, pixels[i]};
            fwrite(rgb, 1, 3, preview);
        }
        fclose(preview);
    }
    /* A complete font unload/reload must preserve the rasterized frame. */
    size_t count = (size_t)width * height;
    uint32_t *reference = malloc(count * sizeof(*reference)); assert(reference);
    memcpy(reference, pixels, count * sizeof(*reference));
    for (int reload = 0; reload < 4; ++reload) {
        tiny_font_close(app.font);
        app.font = NULL;
        assert(app_paint(&app, pixels, width, height, width));
        assert(!memcmp(reference, pixels, count * sizeof(*pixels)));
    }
    free(reference);
    free(pixels);
    /* Exercise scrolling and clipping after repeated input. */
    press(&client, XKB_KEY_i);
    for (int i = 0; i < 100; ++i) { press(&client, XKB_KEY_a); press(&client, XKB_KEY_Return); }
    for (int i = 0; i < 150; ++i) press(&client, XKB_KEY_b);
    const int dimensions[][2] = {{1200,840},{1,1},{30,20},{800,560}};
    for (unsigned i = 0; i < sizeof(dimensions)/sizeof(dimensions[0]); ++i) {
        width = dimensions[i][0]; height = dimensions[i][1];
        pixels = calloc((size_t)width * height, sizeof(*pixels)); assert(pixels);
        assert(app_paint(&app,pixels,width,height,width)); free(pixels);
    }
    assert(app.editor.top > 0 && app.editor.left == 0 && app.editor.viewport_columns > 0);
    press(&client, XKB_KEY_Escape);
    press(&client, XKB_KEY_colon); press(&client, XKB_KEY_w); press(&client, XKB_KEY_q); press(&client, XKB_KEY_Return);
    assert(app.editor.quit && !app.running);
    editor_destroy(&app.editor);
    xkb_state_unref(client.xkb_state); xkb_keymap_unref(client.xkb_keymap); xkb_context_unref(client.xkb_context);
    tiny_font_close(app.font);
    unlink(path);
    puts("Keyboard, rendering, scrolling, and save/quit integration passed");
}
