#include "../src/platform/linux/wayland.c"
#include <assert.h>
#include <wayland-server.h>

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

/* Press the key showing symbol at level 0 with Alt (and optionally Num Lock) held. */
static void press_alt(struct wayland_client *client, xkb_keysym_t symbol, bool num_lock)
{
    xkb_mod_index_t alt = xkb_keymap_mod_get_index(client->xkb_keymap, XKB_MOD_NAME_ALT),
                    num = xkb_keymap_mod_get_index(client->xkb_keymap, XKB_MOD_NAME_NUM);
    xkb_state_update_mask(client->xkb_state, 1u << alt, 0, num_lock ? 1u << num : 0, 0, 0, 0);
    for (xkb_keycode_t code = xkb_keymap_min_keycode(client->xkb_keymap);
         code <= xkb_keymap_max_keycode(client->xkb_keymap); ++code)
        if (xkb_state_key_get_one_sym(client->xkb_state, code) == symbol) {
            handle_key(client, code - 8);
            xkb_state_update_mask(client->xkb_state, 0, 0, 0, 0, 0, 0);
            return;
        }
    assert(!"Could not map keysym");
}

/* Alt+0 to Alt+9 reach the app as KEY_PAGE from the number row and the keypad. An explicit
 * file has no pages, so each press reports that instead of typing. */
static void page_keys(void)
{
    struct app app = {.running = true};
    assert(editor_open(&app.editor, NULL));
    struct wayland_client client = {.app = &app};
    client.xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS); assert(client.xkb_context);
    const char *layouts[] = {"us", "fr"};
    for (unsigned l = 0; l < 2; ++l) {
        struct xkb_rule_names names = {.layout = layouts[l]};
        client.xkb_keymap = xkb_keymap_new_from_names(client.xkb_context, &names,
                                                      XKB_KEYMAP_COMPILE_NO_FLAGS);
        assert(client.xkb_keymap);
        client.xkb_state = xkb_state_new(client.xkb_keymap); assert(client.xkb_state);
        /* The key left of 4 on the number row: "3" in US, a quote in AZERTY. */
        const xkb_keysym_t row = l ? XKB_KEY_quotedbl : XKB_KEY_3;
        /* And 0 on the number row: "0" in US, a with grave in AZERTY. */
        const xkb_keysym_t zero = l ? XKB_KEY_agrave : XKB_KEY_0;
        const xkb_keysym_t keys[] = {row, XKB_KEY_KP_3, XKB_KEY_KP_Next, zero, XKB_KEY_KP_0,
                                     XKB_KEY_KP_Insert};
        for (unsigned i = 0; i < 6; ++i) {
            app.editor.message[0] = 0;
            press_alt(&client, keys[i], i == 1 || i == 4);
            assert(strstr(app.editor.message, "not a single file") && !app.editor.length);
        }
        xkb_state_unref(client.xkb_state); xkb_keymap_unref(client.xkb_keymap);
    }
    xkb_context_unref(client.xkb_context);
    editor_destroy(&app.editor);
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

static void decorations(void)
{
    struct app app;
    assert(app_open(&app, NULL));
    struct wayland_client client = {.app = &app, .pending_csd = true};
    apply_decorations(&client); /* no decoration protocol: client supplies a title bar */
    assert(app.client_decorated && app.dirty);
    decoration_configure(&client, NULL, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    assert(app.client_decorated); /* mode is deferred until xdg_surface.configure */
    apply_decorations(&client);
    assert(!app.client_decorated);
    assert(window_buffer_format(&app) == WL_SHM_FORMAT_XRGB8888);
    decoration_configure(&client, NULL, ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
    apply_decorations(&client);
    assert(app.client_decorated);
    assert(window_buffer_format(&app) == WL_SHM_FORMAT_ARGB8888);
    uint32_t tile = XDG_TOPLEVEL_STATE_TILED_LEFT;
    struct wl_array states = {.size = sizeof(tile), .alloc = sizeof(tile), .data = &tile};
    toplevel_configure(&client, NULL, 800, 560, &states);
    assert(app.tiled && window_buffer_format(&app) == WL_SHM_FORMAT_XRGB8888);
    states.size = 0;
    toplevel_configure(&client, NULL, 800, 560, &states);
    assert(!app.tiled && window_buffer_format(&app) == WL_SHM_FORMAT_ARGB8888);
    assert(resize_edge(&client, 1, 1) == XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT);
    assert(resize_edge(&client, 799, 559) == XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT);
    assert(resize_edge(&client, 200, 20) == XDG_TOPLEVEL_RESIZE_EDGE_NONE);
    app.maximized = true;
    assert(window_buffer_format(&app) == WL_SHM_FORMAT_XRGB8888);
    assert(resize_edge(&client, 1, 1) == XDG_TOPLEVEL_RESIZE_EDGE_NONE);
    app.maximized = false;
    client.fullscreen = true;
    apply_decorations(&client);
    assert(!app.client_decorated);
    client.fullscreen = false;
    apply_decorations(&client);
    assert(app.client_decorated);
    app.title_height = 40;
    app.view_width = 800;
    client.pointer_x = 780;
    client.pointer_y = 20;
    pointer_enter(&client, NULL, 99, NULL, wl_fixed_from_int(780), wl_fixed_from_int(20));
    assert(app.title_hover == TITLE_CLOSE);
    app.dirty = false;
    pointer_motion(&client, NULL, 1, wl_fixed_from_int(781), wl_fixed_from_int(20));
    assert(!app.dirty); /* moving inside one button needs no extra repaint */
    pointer_button(&client, NULL, 100, 2, 0x110, WL_POINTER_BUTTON_STATE_PRESSED);
    assert(app.title_pressed == TITLE_CLOSE && app.dirty);
    pointer_motion(&client, NULL, 3, wl_fixed_from_int(400), wl_fixed_from_int(20));
    assert(app.title_hover == TITLE_NONE && app.title_pressed == TITLE_CLOSE);
    pointer_button(&client, NULL, 101, 4, 0x110, WL_POINTER_BUTTON_STATE_RELEASED);
    assert(app.title_pressed == TITLE_NONE && app.running);
    pointer_motion(&client, NULL, 5, wl_fixed_from_int(780), wl_fixed_from_int(20));
    assert(app.title_hover == TITLE_CLOSE);
    app.dirty = false;
    pointer_leave(&client, NULL, 102, NULL);
    assert(app.title_hover == TITLE_NONE && app.dirty);
    client.dragging = true;
    pointer_button(&client, NULL, 0, 90, 0x110, WL_POINTER_BUTTON_STATE_RELEASED);
    assert(!client.dragging); /* releasing a text selection over the header ends the drag */
    assert(editor_replace(&app.editor, 0, 0, "notes", 5));
    assert(decoration_button(&client, 1, 100, 0x110, WL_POINTER_BUTTON_STATE_PRESSED));
    assert(app.running);
    assert(decoration_button(&client, 2, 110, 0x110, WL_POINTER_BUTTON_STATE_RELEASED));
    assert(app.running && app.editor.modified && strstr(app.editor.message, "Unsaved"));
    assert(decoration_button(&client, 3, 120, 0x110, WL_POINTER_BUTTON_STATE_PRESSED));
    client.pointer_y = 100;
    assert(decoration_button(&client, 4, 130, 0x110, WL_POINTER_BUTTON_STATE_RELEASED));
    assert(app.running && app.title_pressed == TITLE_NONE); /* release outside cancels */
    client.pointer_y = 20;
    app.editor.modified = false;
    assert(decoration_button(&client, 5, 140, 0x110, WL_POINTER_BUTTON_STATE_PRESSED));
    assert(decoration_button(&client, 6, 150, 0x110, WL_POINTER_BUTTON_STATE_RELEASED));
    assert(!app.running);
    app_close(&app);
}

struct resize_requests {
    unsigned resizes, cursors;
    uint32_t serial, edge, shape;
};
static int resize_dispatch(const void *implementation, void *target, uint32_t opcode,
                           const struct wl_message *message, union wl_argument *arguments)
{
    (void)implementation;
    (void)opcode;
    struct resize_requests *requests = wl_resource_get_user_data(target);
    if (!strcmp(message->name, "resize")) {
        assert(arguments[1].u == requests->serial);
        assert(arguments[2].u == requests->edge);
        ++requests->resizes;
    } else {
        assert(!strcmp(message->name, "set_shape"));
        assert(arguments[0].u == 99); /* the pointer-enter serial survives button events */
        assert(arguments[1].u == requests->shape);
        ++requests->cursors;
    }
    return 0;
}
static void resize_protocol(void)
{
    int sockets[2];
    assert(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets));
    struct wl_display *server = wl_display_create();
    assert(server);
    struct wl_client *peer = wl_client_create(server, sockets[0]);
    struct wl_display *display = wl_display_connect_to_fd(sockets[1]);
    assert(peer && display);
    struct app app;
    assert(app_open(&app, NULL));
    app.client_decorated = true;
    app.title_height = 40;
    app.view_width = 800;
    assert(editor_replace(&app.editor, 0, 0, "notes", 5));
    app.editor.cursor = 2;
    struct wayland_client client = {.app = &app, .cursor_serial = 99};
    struct resize_requests requests = {0};
    struct wl_proxy *seat = wl_proxy_create((struct wl_proxy *)display, &wl_seat_interface);
    struct wl_proxy *top = wl_proxy_create((struct wl_proxy *)display, &xdg_toplevel_interface);
    struct wl_proxy *cursor = wl_proxy_create((struct wl_proxy *)display, &wp_cursor_shape_device_v1_interface);
    assert(seat && top && cursor);
    client.seat = (struct wl_seat *)seat;
    client.toplevel = (struct xdg_toplevel *)top;
    client.cursor_device = (struct wp_cursor_shape_device_v1 *)cursor;
    struct wl_resource *seat_resource = wl_resource_create(peer, &wl_seat_interface, 1, wl_proxy_get_id(seat));
    struct wl_resource *top_resource = wl_resource_create(peer, &xdg_toplevel_interface, 1, wl_proxy_get_id(top));
    struct wl_resource *cursor_resource = wl_resource_create(peer, &wp_cursor_shape_device_v1_interface, 1, wl_proxy_get_id(cursor));
    assert(seat_resource && top_resource && cursor_resource);
    wl_resource_set_dispatcher(top_resource, resize_dispatch, &requests, &requests, NULL);
    wl_resource_set_dispatcher(cursor_resource, resize_dispatch, &requests, &requests, NULL);
    const struct { int x, y; uint32_t edge, shape; } positions[] = {
        {400, 7, XDG_TOPLEVEL_RESIZE_EDGE_TOP, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_N_RESIZE},
        {400, 553, XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_S_RESIZE},
        {7, 280, XDG_TOPLEVEL_RESIZE_EDGE_LEFT, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_W_RESIZE},
        {793, 280, XDG_TOPLEVEL_RESIZE_EDGE_RIGHT, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_E_RESIZE},
        {7, 19, XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NW_RESIZE},
        {781, 7, XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NE_RESIZE},
        {19, 553, XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SW_RESIZE},
        {793, 541, XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SE_RESIZE},
    };
    for (unsigned i = 0; i < sizeof(positions) / sizeof(positions[0]); ++i) {
        requests.serial = 100 + i;
        requests.edge = positions[i].edge;
        requests.shape = positions[i].shape;
        pointer_motion(&client, NULL, i, wl_fixed_from_int(positions[i].x), wl_fixed_from_int(positions[i].y));
        pointer_button(&client, NULL, requests.serial, i, 0x110, WL_POINTER_BUTTON_STATE_PRESSED);
        assert(wl_display_flush(display) >= 0);
        assert(wl_event_loop_dispatch(wl_display_get_event_loop(server), 0) >= 0);
        assert(requests.resizes == i + 1 && requests.cursors == i + 1);
        pointer_button(&client, NULL, requests.serial, i, 0x110, WL_POINTER_BUTTON_STATE_RELEASED);
        assert(!client.dragging && app.editor.cursor == 2 && !strcmp(app.editor.text, "notes"));
    }
    requests.shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
    pointer_motion(&client, NULL, 10, wl_fixed_from_int(400), wl_fixed_from_int(280));
    assert(wl_display_flush(display) >= 0);
    assert(wl_event_loop_dispatch(wl_display_get_event_loop(server), 0) >= 0);
    assert(requests.cursors == 9 && requests.resizes == 8);
    pointer_motion(&client, NULL, 11, wl_fixed_from_int(401), wl_fixed_from_int(280));
    assert(wl_display_flush(display) >= 0);
    assert(wl_event_loop_dispatch(wl_display_get_event_loop(server), 0) >= 0);
    assert(requests.cursors == 9); /* unchanged shapes don't generate more requests */
    wl_proxy_destroy(cursor);
    wl_proxy_destroy(top);
    wl_proxy_destroy(seat);
    wl_display_disconnect(display);
    wl_display_destroy_clients(server);
    wl_display_destroy(server);
    app_close(&app);
}

/* Capture real wl_region requests on an isolated in-process protocol peer. */
struct shape_region { bool pixels[32 * 32]; };
struct shape_requests {
    struct shape_region opaque, input;
    unsigned creates, updates;
};
static void shape_destroy(struct wl_client *client, struct wl_resource *resource)
{
    (void)client;
    wl_resource_destroy(resource);
}
static void shape_freed(struct wl_resource *resource)
{
    free(wl_resource_get_user_data(resource));
}
static void shape_add(struct wl_client *client, struct wl_resource *resource,
                      int32_t x, int32_t y, int32_t width, int32_t height)
{
    (void)client;
    assert(x >= 0 && y >= 0 && width > 0 && height > 0 && x + width <= 32 && y + height <= 32);
    struct shape_region *region = wl_resource_get_user_data(resource);
    for (int row = y; row < y + height; ++row)
        for (int col = x; col < x + width; ++col)
            region->pixels[row * 32 + col] = true;
}
static const struct wl_region_interface shape_region_impl = {
    .destroy = shape_destroy, .add = shape_add,
};
static void shape_create(struct wl_client *client, struct wl_resource *compositor, uint32_t id)
{
    struct shape_requests *requests = wl_resource_get_user_data(compositor);
    ++requests->creates;
    struct wl_resource *region = wl_resource_create(client, &wl_region_interface, 1, id);
    struct shape_region *data = calloc(1, sizeof(*data));
    assert(region && data);
    wl_resource_set_implementation(region, &shape_region_impl, data, shape_freed);
}
static void shape_opaque(struct wl_client *client, struct wl_resource *surface, struct wl_resource *region)
{
    (void)client;
    struct shape_requests *requests = wl_resource_get_user_data(surface);
    assert(region);
    requests->opaque = *(struct shape_region *)wl_resource_get_user_data(region);
    ++requests->updates;
}
static void shape_input(struct wl_client *client, struct wl_resource *surface, struct wl_resource *region)
{
    (void)client;
    struct shape_requests *requests = wl_resource_get_user_data(surface);
    if (region)
        requests->input = *(struct shape_region *)wl_resource_get_user_data(region);
    else
        for (unsigned i = 0; i < 32 * 32; ++i)
            requests->input.pixels[i] = true;
}
static void region_protocol(void)
{
    int sockets[2];
    assert(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets));
    struct wl_display *server = wl_display_create();
    assert(server);
    struct wl_client *peer = wl_client_create(server, sockets[0]);
    struct wl_display *display = wl_display_connect_to_fd(sockets[1]);
    assert(peer && display);
    struct wl_proxy *compositor = wl_proxy_create((struct wl_proxy *)display, &wl_compositor_interface);
    struct wl_proxy *surface = wl_proxy_create((struct wl_proxy *)display, &wl_surface_interface);
    assert(compositor && surface);
    struct wl_resource *cr = wl_resource_create(peer, &wl_compositor_interface, 1, wl_proxy_get_id(compositor));
    struct wl_resource *sr = wl_resource_create(peer, &wl_surface_interface, 1, wl_proxy_get_id(surface));
    struct shape_requests requests = {0};
    const struct wl_compositor_interface ci = {.create_region = shape_create};
    const struct wl_surface_interface si = {.set_opaque_region = shape_opaque, .set_input_region = shape_input};
    assert(cr && sr);
    wl_resource_set_implementation(cr, &ci, &requests, NULL);
    wl_resource_set_implementation(sr, &si, &requests, NULL);
    struct app app;
    assert(app_open(&app, NULL));
    app.width = 32;
    app.height = 28;
    struct wayland_client client = {.app = &app, .compositor = (struct wl_compositor *)compositor,
                                     .surface = (struct wl_surface *)surface};
    for (unsigned state = 0; state < 5; ++state) {
        app.client_decorated = state != 3;
        app.maximized = state == 1;
        app.tiled = state == 2;
        if (state == 4) {
            app.width = 24;
            app.height = 18;
        }
        unsigned updates = requests.updates;
        assert(update_surface_regions(&client));
        assert(update_surface_regions(&client)); /* cached until shape changes */
        assert(wl_display_flush(display) >= 0);
        assert(wl_event_loop_dispatch(wl_display_get_event_loop(server), 0) >= 0);
        if (state != 2 && state != 3)
            assert(requests.updates == updates + 1);
        else
            assert(requests.updates == updates); /* same square region */
        bool rounded = state == 0 || state == 4;
        assert(requests.input.pixels[0] != rounded && requests.opaque.pixels[0] != rounded);
        assert(requests.input.pixels[12]);
        const unsigned scales[] = {120, 150, 180, 240};
        uint32_t pixels[64 * 64];
        for (unsigned i = 0; i < sizeof(scales) / sizeof(scales[0]); ++i) {
            app.scale120 = scales[i];
            int width = (app.width * (int)scales[i] + 60) / 120;
            int height = (app.height * (int)scales[i] + 60) / 120;
            assert(app_paint(&app, pixels, width, height, 64));
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x) {
                    int lx = x * 120 / (int)scales[i], ly = y * 120 / (int)scales[i];
                    unsigned alpha = pixels[y * 64 + x] >> 24;
                    assert(!requests.opaque.pixels[ly * 32 + lx] || alpha == 255);
                    assert(!alpha || requests.input.pixels[ly * 32 + lx]);
                }
        }
    }
    wl_proxy_destroy(surface);
    wl_proxy_destroy(compositor);
    wl_display_disconnect(display);
    wl_display_destroy_clients(server);
    wl_display_destroy(server);
    app_close(&app);
}

static void count_buffer_destroy(struct wl_resource *resource)
{
    unsigned *destroyed = wl_resource_get_user_data(resource);
    ++*destroyed;
}
static void pending_buffer_cleanup(void)
{
    int sockets[2];
    assert(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets));
    struct wl_display *server = wl_display_create();
    assert(server);
    struct wl_client *peer = wl_client_create(server, sockets[0]);
    struct wl_display *display = wl_display_connect_to_fd(sockets[1]);
    assert(peer && display);
    unsigned destroyed = 0;
    struct wayland_client client = {.pending_buffers = 2};
    const struct wl_buffer_interface implementation = {.destroy = shape_destroy};
    for (unsigned i = 0; i < 2; ++i) {
        struct wl_proxy *proxy = wl_proxy_create((struct wl_proxy *)display, &wl_buffer_interface);
        assert(proxy);
        struct wl_resource *resource = wl_resource_create(peer, &wl_buffer_interface, 1, wl_proxy_get_id(proxy));
        assert(resource);
        wl_resource_set_implementation(resource, &implementation, &destroyed, count_buffer_destroy);
        client.buffers[i] = (struct wl_buffer *)proxy;
    }
    buffer_release(&client, client.buffers[0]);
    assert(!client.buffers[0] && client.pending_buffers == 1);
    destroy_buffers(&client); /* Quit before the compositor releases the other frame. */
    assert(!client.buffers[1] && client.pending_buffers == 0);
    destroy_buffers(&client);
    assert(wl_display_flush(display) >= 0);
    assert(wl_event_loop_dispatch(wl_display_get_event_loop(server), 0) >= 0);
    assert(destroyed == 2);
    wl_display_disconnect(display);
    wl_display_destroy_clients(server);
    wl_display_destroy(server);
}

int main(int argc, char **argv)
{
    frame_limit();
    pending_buffer_cleanup();
    page_keys();
    decorations();
    resize_protocol();
    region_protocol();
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
