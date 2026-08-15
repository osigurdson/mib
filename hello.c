#define _GNU_SOURCE
#include <errno.h>
#include "tiny-font.h"
#include <limits.h>
#include <poll.h>
#include <time.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include "editor.h"
#ifdef __GLIBC__
#include <malloc.h>
#endif
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"

struct app {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *wm;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct wp_viewporter *viewporter;
    struct wp_viewport *viewport;
    struct wp_fractional_scale_manager_v1 *scale_manager;
    struct wp_fractional_scale_v1 *fractional_scale;
    uint32_t scale120;
    bool configured;
    struct tiny_font *font;
    int width, height;
    struct editor editor;
    struct wl_seat *seat;
    struct wl_keyboard *keyboard;
    struct xkb_context *xkb_context;
    struct xkb_keymap *xkb_keymap;
    struct xkb_state *xkb_state;
    uint32_t repeat_key;
    int repeat_rate, repeat_delay;
    int64_t repeat_at;
    unsigned pending_buffers;
    bool dirty;
    bool running, failed;
};

static void close_font(struct app *app)
{
    tiny_font_close(app->font);
    app->font = NULL;
}

static bool init_font(struct app *app)
{
    if (!app->font) app->font = tiny_font_open();
    return app->font != NULL;
}

static uint32_t blend(uint32_t background, unsigned alpha)
{
    const uint32_t foreground = 0xffe5edf5;
    uint32_t pixel = 0xff000000;
    for (unsigned shift = 0; shift < 24; shift += 8) {
        unsigned fg = (foreground >> shift) & 255;
        unsigned bg = (background >> shift) & 255;
        pixel |= ((fg * alpha + bg * (255 - alpha) + 127) / 255) << shift;
    }
    return pixel;
}

static void rectangle(uint32_t *pixels, int width, int height,
                      int x, int y, int w, int h, uint32_t color)
{
    for (int row = y < 0 ? 0 : y; row < height && row < y + h; ++row)
        for (int col = x < 0 ? 0 : x; col < width && col < x + w; ++col)
            pixels[(size_t)row * (size_t)width + (size_t)col] = color;
}

static bool glyph(struct app *app, uint32_t *pixels, int width, int height,
                  uint32_t cp, int x, int baseline, int clip_bottom)
{
    const struct tiny_glyph *bitmap = tiny_font_glyph(app->font, cp);
    if (!bitmap) return false;
    for (int row = 0; row < bitmap->height; ++row) {
        int y = baseline - bitmap->top + row;
        if (y < 0 || y >= height || y >= clip_bottom) continue;
        const unsigned char *coverage = bitmap->alpha + (size_t)row * bitmap->width;
        for (int col = 0; col < bitmap->width; ++col) {
            int px = x + bitmap->left + col;
            if (px < 0 || px >= width) continue;
            size_t offset = (size_t)y * (size_t)width + (size_t)px;
            pixels[offset] = blend(pixels[offset], coverage[col]);
        }
    }
    return true;
}

static bool label(struct app *app, uint32_t *pixels, int width, int height,
                  const char *text, int x, int baseline, int cell)
{
    size_t length = strlen(text), offset = 0;
    while (offset < length && x < width - cell) {
        uint32_t cp = editor_codepoint(text, length, &offset);
        if (cp < 32 || cp == 127) cp = '?';
        if (!glyph(app, pixels, width, height, cp, x, baseline, height)) return false;
        x += cell;
    }
    return true;
}

static bool paint(struct app *app, uint32_t *pixels, int width, int height)
{
    struct editor *ed = &app->editor;
    unsigned size = (unsigned)((18 * 64 * (int64_t)app->scale120 + 60) / 120);
    if (!tiny_font_set_size(app->font, size)) return false;
    struct tiny_font_metrics metrics = tiny_font_metrics(app->font);
    int cell = metrics.advance;
    int ascender = metrics.ascender;
    int line_height = metrics.line_height + 2;
    int padding = (int)((8 * (int64_t)app->scale120 + 60) / 120);
    int status_y = height - 2 * line_height - padding;
    int rows = (status_y - padding) / line_height;
    int columns = (width - 2 * padding) / cell;
    if (rows < 1) rows = 1;
    if (columns < 1) columns = 1;
    size_t cursor_row = 0;
    for (size_t i = 0; i < ed->cursor; ++i) if (ed->text[i] == '\n') ++cursor_row;
    size_t cursor_column = editor_column(ed, ed->cursor);
    if (cursor_row < ed->top) ed->top = cursor_row;
    if (cursor_row - ed->top >= (size_t)rows) ed->top = cursor_row - (size_t)rows + 1;
    if (cursor_column < ed->left) ed->left = cursor_column;
    if (cursor_column - ed->left >= (size_t)columns) ed->left = cursor_column - (size_t)columns + 1;
    for (size_t i = 0; i < (size_t)width * (size_t)height; ++i) pixels[i] = 0xff18202b;

    size_t start = 0;
    for (size_t row = 0; row < ed->top && start < ed->length; ++row) {
        start = editor_line_end(ed, start);
        if (start < ed->length) ++start;
    }
    for (int row = 0; row < rows; ++row) {
        size_t end = editor_line_end(ed, start);
        size_t offset = start, column = 0;
        while (offset < end) {
            uint32_t cp = editor_codepoint(ed->text, end, &offset);
            size_t advance = cp == '\t' ? 4 - column % 4 : 1;
            if (column >= ed->left && column - ed->left >= (size_t)columns) break;
            if (column >= ed->left && cp != '\t') {
                int x = padding + (int)(column - ed->left) * cell;
                int y = padding + row * line_height;
                if (cp < 32 || cp == 127) cp = '?';
                if (!glyph(app, pixels, width, height, cp, x, y + ascender, status_y)) return false;
            }
            column += advance;
        }
        if (end == ed->length) break;
        start = end + 1;
    }
    int cursor_x = padding + (int)(cursor_column - ed->left) * cell;
    int cursor_y = padding + (int)(cursor_row - ed->top) * line_height;
    if (!ed->command_active) {
        if (ed->mode == INSERT)
            rectangle(pixels, width, height, cursor_x, cursor_y, 2, line_height, 0xff8fbcbb);
        else
            rectangle(pixels, width, height, cursor_x, cursor_y + line_height - 3,
                      cell, 3, 0xff8fbcbb);
    }
    rectangle(pixels, width, height, 0, status_y, width, line_height + 2 * padding, 0xff283443);
    char status[512];
    snprintf(status, sizeof(status), "%s  \uf15b %s%s  |  %zu:%zu",
             ed->mode == INSERT ? "\uf040 INSERT" : "NORMAL",
             ed->filename ? ed->filename : "[No Name]", ed->modified ? " [+] \uf111" : "",
             cursor_row + 1, cursor_column + 1);
    if (!label(app, pixels, width, height, status, padding, status_y + ascender, cell)) return false;
    char prompt[1100];
    if (ed->command_active) snprintf(prompt, sizeof(prompt), ":%s_", ed->command);
    else snprintf(prompt, sizeof(prompt), "%s", ed->message[0] ? ed->message :
                  "i insert  Esc normal  hjkl move  :w :q :wq");
    return label(app, pixels, width, height, prompt, padding,
                 status_y + line_height + ascender, cell);
}

static void buffer_release(void *data, struct wl_buffer *buffer)
{
    struct app *app = data;
    --app->pending_buffers;
    wl_buffer_destroy(buffer);
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

static bool draw(struct app *app)
{
    /* Coalesce edits and bound memory while the compositor reads old frames. */
    if (!app->dirty || app->pending_buffers >= 2) return true;
    /* Surface dimensions are logical; buffers and font sizes use physical pixels. */
    int64_t physical_width = ((int64_t)app->width * app->scale120 + 60) / 120;
    int64_t physical_height = ((int64_t)app->height * app->scale120 + 60) / 120;
    if (physical_width < 1) physical_width = 1;
    if (physical_height < 1) physical_height = 1;
    /* wl_shm uses signed 32-bit sizes and strides. */
    if (physical_height > INT_MAX || physical_width > INT_MAX / 4 / physical_height) {
        fprintf(stderr, "Window dimensions exceed the shared-memory limit\n");
        return false;
    }
    int width = (int)physical_width, height = (int)physical_height;
    int stride = width * 4;
    int size = stride * height;
    int fd = memfd_create("tiny-vi", MFD_CLOEXEC);
    if (fd < 0) {
        perror("memfd_create");
        return false;
    }
    if (ftruncate(fd, size) < 0) {
        perror("ftruncate");
        close(fd);
        return false;
    }
    uint32_t *pixels = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE,
                            MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return false;
    }
    bool painted = (app->font || init_font(app)) && paint(app, pixels, width, height);
    /* Retain only the bounded glyph cache between edits. */
    tiny_font_end_frame(app->font);
#ifdef __GLIBC__
    malloc_trim(0);
#endif
    if (!painted) {
        fprintf(stderr, "Could not rasterize the bundled font\n");
        munmap(pixels, (size_t)size);
        close(fd);
        return false;
    }
    struct wl_shm_pool *pool = wl_shm_create_pool(app->shm, fd, size);
    struct wl_buffer *buffer = pool ? wl_shm_pool_create_buffer(
        pool, 0, width, height, stride, WL_SHM_FORMAT_XRGB8888) : NULL;
    if (pool) wl_shm_pool_destroy(pool);
    /* The compositor keeps its own mapping; our pixels are never reused. */
    munmap(pixels, (size_t)size);
    close(fd);
    if (!buffer) {
        fprintf(stderr, "Could not create a Wayland shared-memory buffer\n");
        return false;
    }
    wl_buffer_add_listener(buffer, &buffer_listener, app);
    ++app->pending_buffers;
    if (app->viewport) {
        wp_viewport_set_destination(app->viewport, app->width, app->height);
    } else if (wl_surface_get_version(app->surface) >= 3) {
        wl_surface_set_buffer_scale(app->surface, (int32_t)(app->scale120 / 120));
    }
    wl_surface_attach(app->surface, buffer, 0, 0);
    wl_surface_damage(app->surface, 0, 0, app->width, app->height);
    wl_surface_commit(app->surface);
    app->dirty = false;
    return true;
}

static void update_scale(struct app *app, uint32_t scale120)
{
    if (!scale120 || scale120 == app->scale120) return;
    app->scale120 = scale120;
    fprintf(stderr, "Render scale: %.2fx\n", scale120 / 120.0);
    app->dirty = true;
}

static void fractional_preferred(void *data, struct wp_fractional_scale_v1 *scale,
                                 uint32_t scale120)
{
    (void)scale;
    update_scale(data, scale120);
}

static const struct wp_fractional_scale_v1_listener scale_listener = {
    .preferred_scale = fractional_preferred,
};

static void surface_enter(void *data, struct wl_surface *surface, struct wl_output *output)
{
    (void)data; (void)surface; (void)output;
}

static void surface_leave(void *data, struct wl_surface *surface, struct wl_output *output)
{
    (void)data; (void)surface; (void)output;
}

static void surface_scale(void *data, struct wl_surface *surface, int32_t scale)
{
    (void)surface;
    struct app *app = data;
    if (!app->fractional_scale && scale > 0 && (uint32_t)scale <= UINT32_MAX / 120)
        update_scale(app, (uint32_t)scale * 120);
}

static void surface_transform(void *data, struct wl_surface *surface, uint32_t transform)
{
    (void)data; (void)surface; (void)transform;
}

static const struct wl_surface_listener wl_surface_listener = {
    .enter = surface_enter,
    .leave = surface_leave,
    .preferred_buffer_scale = surface_scale,
    .preferred_buffer_transform = surface_transform,
};

static void surface_configure(void *data, struct xdg_surface *surface, uint32_t serial)
{
    struct app *app = data;
    xdg_surface_ack_configure(surface, serial);
    app->configured = true;
    /* Paint once after the current batch of protocol events. */
}

static const struct xdg_surface_listener surface_listener = {
    .configure = surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel,
                              int32_t width, int32_t height, struct wl_array *states)
{
    (void)toplevel;
    (void)states;
    struct app *app = data;
    if (width > 0 && width != app->width) { app->width = width; app->dirty = true; }
    if (height > 0 && height != app->height) { app->height = height; app->dirty = true; }
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
    (void)toplevel;
    struct app *app = data;
    if (app->editor.modified) {
        snprintf(app->editor.message, sizeof(app->editor.message), "Unsaved changes (:w to save, :q! to discard)");
        app->dirty = true;
    } else app->running = false;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
};

static void wm_ping(void *data, struct xdg_wm_base *wm, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(wm, serial);
}

static const struct xdg_wm_base_listener wm_listener = {.ping = wm_ping};

static void stop_repeat(struct app *app)
{
    app->repeat_key = 0;
}

static void handle_key(struct app *app, uint32_t key)
{
    if (!app->xkb_state) return;
    xkb_keysym_t symbol = xkb_state_key_get_one_sym(app->xkb_state, key + 8);
    enum editor_key action = KEY_TEXT;
    switch (symbol) {
    case XKB_KEY_Escape: action = KEY_ESCAPE; break;
    case XKB_KEY_Return: case XKB_KEY_KP_Enter: action = KEY_ENTER; break;
    case XKB_KEY_BackSpace: action = KEY_BACKSPACE; break;
    case XKB_KEY_Delete: action = KEY_DELETE; break;
    case XKB_KEY_Left: action = KEY_LEFT; break;
    case XKB_KEY_Right: action = KEY_RIGHT; break;
    case XKB_KEY_Up: action = KEY_UP; break;
    case XKB_KEY_Down: action = KEY_DOWN; break;
    case XKB_KEY_Tab: action = KEY_TAB; break;
    default: break;
    }
    char text[64] = {0};
    if (action == KEY_TEXT) {
        int length = xkb_state_key_get_utf8(app->xkb_state, key + 8, text, sizeof(text));
        if (length <= 0 || length >= (int)sizeof(text) || (unsigned char)text[0] < 32 ||
            (unsigned char)text[0] == 127) return;
    }
    editor_input(&app->editor, action, text);
    app->dirty = true;
    if (app->editor.quit) app->running = false;
}

static void keyboard_keymap(void *data, struct wl_keyboard *keyboard,
                            uint32_t format, int32_t fd, uint32_t size)
{
    (void)keyboard;
    struct app *app = data;
    stop_repeat(app);
    struct xkb_keymap *keymap = NULL;
    if (format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && size) {
        char *mapping = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (mapping != MAP_FAILED) {
            if (mapping[size - 1] == '\0')
                keymap = xkb_keymap_new_from_string(app->xkb_context, mapping,
                    XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
            munmap(mapping, size);
        }
    }
    close(fd);
    struct xkb_state *state = keymap ? xkb_state_new(keymap) : NULL;
    if (!state) {
        xkb_keymap_unref(keymap);
        snprintf(app->editor.message, sizeof(app->editor.message), "Could not load keyboard mapping");
        app->dirty = true;
        return;
    }
    xkb_state_unref(app->xkb_state);
    xkb_keymap_unref(app->xkb_keymap);
    app->xkb_keymap = keymap;
    app->xkb_state = state;
#ifdef __GLIBC__
    /* The keymap compiler uses temporary allocations that are now free. */
    malloc_trim(0);
#endif
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                           struct wl_surface *surface, struct wl_array *keys)
{
    (void)keyboard; (void)serial; (void)surface; (void)keys;
    stop_repeat(data);
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                           struct wl_surface *surface)
{
    (void)keyboard; (void)serial; (void)surface;
    struct app *app = data;
    stop_repeat(app);
    if (app->xkb_state) xkb_state_update_mask(app->xkb_state, 0, 0, 0, 0, 0, 0);
}

static int64_t now_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                         uint32_t time, uint32_t key, uint32_t state)
{
    (void)keyboard; (void)serial; (void)time;
    struct app *app = data;
    if (state == WL_KEYBOARD_KEY_STATE_RELEASED) {
        if (app->repeat_key == key + 8) stop_repeat(app);
        return;
    }
    handle_key(app, key);
    if (app->xkb_keymap && app->repeat_rate > 0 &&
        xkb_keymap_key_repeats(app->xkb_keymap, key + 8)) {
        app->repeat_key = key + 8;
        app->repeat_at = now_ms() + app->repeat_delay;
    }
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                               uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group)
{
    (void)keyboard; (void)serial;
    struct app *app = data;
    if (app->xkb_state) xkb_state_update_mask(app->xkb_state, depressed, latched, locked, 0, 0, group);
}

static void keyboard_repeat(void *data, struct wl_keyboard *keyboard, int32_t rate, int32_t delay)
{
    (void)keyboard;
    struct app *app = data;
    app->repeat_rate = rate;
    app->repeat_delay = delay;
    stop_repeat(app);
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap, .enter = keyboard_enter, .leave = keyboard_leave,
    .key = keyboard_key, .modifiers = keyboard_modifiers, .repeat_info = keyboard_repeat,
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities)
{
    struct app *app = data;
    if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && !app->keyboard) {
        app->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(app->keyboard, &keyboard_listener, app);
    } else if (!(capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && app->keyboard) {
        wl_keyboard_release(app->keyboard);
        app->keyboard = NULL;
        stop_repeat(app);
    }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name)
{
    (void)data; (void)seat; (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities, .name = seat_name,
};

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface, uint32_t version)
{
    struct app *app = data;
    if (strcmp(interface, wl_compositor_interface.name) == 0)
        app->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, version < 6 ? version : 6);
    else if (strcmp(interface, wl_seat_interface.name) == 0 && !app->seat) {
        app->seat = wl_registry_bind(registry, name, &wl_seat_interface, version < 5 ? version : 5);
        wl_seat_add_listener(app->seat, &seat_listener, app);
    }
    else if (strcmp(interface, wl_shm_interface.name) == 0)
        app->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    else if (strcmp(interface, wp_viewporter_interface.name) == 0)
        app->viewporter = wl_registry_bind(registry, name, &wp_viewporter_interface, 1);
    else if (strcmp(interface, wp_fractional_scale_manager_v1_interface.name) == 0)
        app->scale_manager = wl_registry_bind(registry, name, &wp_fractional_scale_manager_v1_interface, 1);
    else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        app->wm = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(app->wm, &wm_listener, app);
    }
}

static void registry_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_remove,
};

int main(int argc, char **argv)
{
    struct app app = {.width = 800, .height = 560, .scale120 = 120, .running = true, .dirty = true};
    int status = EXIT_FAILURE;
    if (argc > 2) {
        fprintf(stderr, "Usage: %s [file]\n", argv[0]);
        return status;
    }
    if (!editor_open(&app.editor, argc == 2 ? argv[1] : NULL)) {
        fprintf(stderr, "%s\n", app.editor.message[0] ? app.editor.message : "Could not open file");
        goto cleanup;
    }
    app.xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!app.xkb_context) goto cleanup;
    app.display = wl_display_connect(NULL);
    if (!app.display) {
        fprintf(stderr, "Cannot connect to Wayland: %s\n", strerror(errno));
        goto cleanup;
    }
    app.registry = wl_display_get_registry(app.display);
    wl_registry_add_listener(app.registry, &registry_listener, &app);
    if (wl_display_roundtrip(app.display) < 0) goto cleanup;
    if (!app.compositor || !app.shm || !app.wm) {
        fprintf(stderr, "Compositor must provide wl_compositor, wl_shm, and xdg_wm_base\n");
        goto cleanup;
    }
    app.surface = wl_compositor_create_surface(app.compositor);
    wl_surface_add_listener(app.surface, &wl_surface_listener, &app);
    if (app.viewporter && app.scale_manager) {
        app.viewport = wp_viewporter_get_viewport(app.viewporter, app.surface);
        app.fractional_scale = wp_fractional_scale_manager_v1_get_fractional_scale(
            app.scale_manager, app.surface);
        wp_fractional_scale_v1_add_listener(app.fractional_scale, &scale_listener, &app);
    }
    app.xdg_surface = xdg_wm_base_get_xdg_surface(app.wm, app.surface);
    xdg_surface_add_listener(app.xdg_surface, &surface_listener, &app);
    app.toplevel = xdg_surface_get_toplevel(app.xdg_surface);
    xdg_toplevel_add_listener(app.toplevel, &toplevel_listener, &app);
    xdg_toplevel_set_title(app.toplevel, "Tiny vi — CPU / Wayland");
    xdg_toplevel_set_app_id(app.toplevel, "tiny-vi");

    /* Initial empty commit asks the compositor to configure the window. */
    wl_surface_commit(app.surface);
    while (app.running) {
        if (wl_display_dispatch_pending(app.display) < 0) { app.failed = true; break; }
        if (!app.running) break;
        if (app.repeat_key && now_ms() >= app.repeat_at) {
            handle_key(&app, app.repeat_key - 8);
            int interval = 1000 / app.repeat_rate;
            app.repeat_at = now_ms() + (interval > 0 ? interval : 1);
        }
        if (!app.running) break;
        if (app.configured && !draw(&app)) { app.failed = true; break; }
        int flush = wl_display_flush(app.display);
        if (flush < 0 && errno != EAGAIN) { app.failed = true; break; }
        struct pollfd socket = {.fd = wl_display_get_fd(app.display),
                                .events = POLLIN | (flush < 0 ? POLLOUT : 0)};
        int timeout = -1;
        if (app.repeat_key) {
            int64_t remaining = app.repeat_at - now_ms();
            timeout = remaining <= 0 ? 0 : remaining > INT_MAX ? INT_MAX : (int)remaining;
        }
        int ready = poll(&socket, 1, timeout);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0 || (socket.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            app.failed = true; break;
        }
        if ((socket.revents & POLLIN) && wl_display_dispatch(app.display) < 0) {
            app.failed = true; break;
        }
    }
    status = app.failed ? EXIT_FAILURE : EXIT_SUCCESS;

cleanup:
    if (app.keyboard) wl_keyboard_destroy(app.keyboard);
    if (app.seat) wl_seat_destroy(app.seat);
    xkb_state_unref(app.xkb_state);
    xkb_keymap_unref(app.xkb_keymap);
    xkb_context_unref(app.xkb_context);
    if (app.toplevel) xdg_toplevel_destroy(app.toplevel);
    if (app.xdg_surface) xdg_surface_destroy(app.xdg_surface);
    if (app.fractional_scale) wp_fractional_scale_v1_destroy(app.fractional_scale);
    if (app.viewport) wp_viewport_destroy(app.viewport);
    if (app.surface) wl_surface_destroy(app.surface);
    if (app.scale_manager) wp_fractional_scale_manager_v1_destroy(app.scale_manager);
    if (app.viewporter) wp_viewporter_destroy(app.viewporter);
    if (app.wm) xdg_wm_base_destroy(app.wm);
    if (app.shm) wl_shm_destroy(app.shm);
    if (app.compositor) wl_compositor_destroy(app.compositor);
    if (app.registry) wl_registry_destroy(app.registry);
    if (app.display) wl_display_disconnect(app.display);
    close_font(&app);
    editor_destroy(&app.editor);
    return status;
}
