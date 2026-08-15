#define main application_main
#include "../hello.c"
#undef main
#include <assert.h>

static void press(struct app *app, xkb_keysym_t symbol)
{
    for (xkb_keycode_t code = xkb_keymap_min_keycode(app->xkb_keymap);
         code <= xkb_keymap_max_keycode(app->xkb_keymap); ++code) {
        for (xkb_level_index_t level = 0; level < 2; ++level) {
            const xkb_keysym_t *symbols;
            int count = xkb_keymap_key_get_syms_by_level(app->xkb_keymap, code, 0, level, &symbols);
            if (count == 1 && symbols[0] == symbol) {
                xkb_mod_index_t shift = xkb_keymap_mod_get_index(app->xkb_keymap, XKB_MOD_NAME_SHIFT);
                xkb_state_update_mask(app->xkb_state, level ? 1u << shift : 0, 0, 0, 0, 0, 0);
                handle_key(app, code - 8);
                return;
            }
        }
    }
    assert(!"Could not map keysym");
}

int main(int argc, char **argv)
{
    struct app app = {.width = 800, .height = 560, .scale120 = 180, .running = true};
    char path[] = "/tmp/tiny-vi-integration-XXXXXX";
    int fd = mkstemp(path); assert(fd >= 0); close(fd);
    assert(editor_open(&app.editor, path));
    assert(init_font(&app));
    app.xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS); assert(app.xkb_context);
    struct xkb_rule_names names = {.layout = "us"};
    app.xkb_keymap = xkb_keymap_new_from_names(app.xkb_context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    assert(app.xkb_keymap);
    app.xkb_state = xkb_state_new(app.xkb_keymap); assert(app.xkb_state);
    press(&app, XKB_KEY_i);
    const char *text = "Tiny vi, directly on Wayland.\nCPU rendering with tiny-font.\n\n";
    for (const char *p = text; *p; ++p) press(&app, *p == '\n' ? XKB_KEY_Return : (xkb_keysym_t)*p);
    assert(!strcmp(app.editor.text, text));
    press(&app, XKB_KEY_Escape);
    press(&app, XKB_KEY_k); press(&app, XKB_KEY_k);
    press(&app, XKB_KEY_colon); press(&app, XKB_KEY_w); press(&app, XKB_KEY_Return);
    assert(!app.editor.modified);
    FILE *file = fopen(path, "rb"); assert(file);
    char saved[256] = {0}; fread(saved, 1, sizeof(saved) - 1, file); fclose(file);
    assert(!strcmp(saved, text));
    int width = 1200, height = 840;
    uint32_t *pixels = calloc((size_t)width * height, sizeof(*pixels)); assert(pixels);
    assert(paint(&app, pixels, width, height));
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
        close_font(&app);
        assert(!app.font);
        assert(init_font(&app));
        assert(paint(&app, pixels, width, height));
        assert(!memcmp(reference, pixels, count * sizeof(*pixels)));
    }
    free(reference);
    free(pixels);
    /* Exercise scrolling and clipping after repeated input. */
    press(&app, XKB_KEY_i);
    for (int i = 0; i < 100; ++i) { press(&app, XKB_KEY_a); press(&app, XKB_KEY_Return); }
    for (int i = 0; i < 150; ++i) press(&app, XKB_KEY_b);
    const int dimensions[][2] = {{1200,840},{1,1},{30,20},{800,560}};
    for (unsigned i = 0; i < sizeof(dimensions)/sizeof(dimensions[0]); ++i) {
        width = dimensions[i][0]; height = dimensions[i][1];
        pixels = calloc((size_t)width * height, sizeof(*pixels)); assert(pixels);
        assert(paint(&app,pixels,width,height)); free(pixels);
    }
    assert(app.editor.top > 0 && app.editor.left > 0);
    press(&app, XKB_KEY_Escape);
    press(&app, XKB_KEY_colon); press(&app, XKB_KEY_w); press(&app, XKB_KEY_q); press(&app, XKB_KEY_Return);
    assert(app.editor.quit && !app.running);
    editor_destroy(&app.editor);
    xkb_state_unref(app.xkb_state); xkb_keymap_unref(app.xkb_keymap); xkb_context_unref(app.xkb_context);
    close_font(&app);
    unlink(path);
    puts("Keyboard, rendering, scrolling, and save/quit integration passed");
}
