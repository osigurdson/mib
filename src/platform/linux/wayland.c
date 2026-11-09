#define _GNU_SOURCE
#include "app.h"
#include "media.h"
#include "fractional-scale-v1-client-protocol.h"
#include "platform.h"
#include "session.h"
#include "viewporter-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "cursor-shape-v1-client-protocol.h"
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

struct wayland_client {
    struct app *app;
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *wm;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct zxdg_decoration_manager_v1 *decoration_manager;
    struct zxdg_toplevel_decoration_v1 *decoration;
    bool pending_csd, fullscreen;
    uint32_t title_click;
    int title_click_x, title_click_y;
    int region_width, region_height, region_radius;
    struct wp_viewporter *viewporter;
    struct wp_viewport *viewport;
    struct wp_fractional_scale_manager_v1 *scale_manager;
    struct wp_fractional_scale_v1 *fractional_scale;
    struct wl_seat *seat;
    struct wl_keyboard *keyboard;
    struct wl_pointer *pointer;
    struct wp_cursor_shape_manager_v1 *cursor_manager;
    struct wp_cursor_shape_device_v1 *cursor_device;
    uint32_t cursor_serial, cursor_shape;
    int pointer_x, pointer_y;
    bool dragging;
    uint32_t serial, last_click;
    int last_click_x, last_click_y;
    struct wl_data_device_manager *data_manager;
    struct wl_data_device *data_device;
    struct clipboard_offer *pending_offer, *selection_offer, *drag_offer, *receive_offer;
    struct clipboard_source *sources;
    int receive_fd, receive_kind;
    bool receive_text; /* whether a uri-list being received may paste text files */
    char *receive_data;
    size_t receive_length, receive_capacity;
    struct xkb_context *xkb_context;
    struct xkb_keymap *xkb_keymap;
    struct xkb_state *xkb_state;
    uint32_t repeat_key;
    int repeat_rate, repeat_delay;
    int64_t repeat_at;
    struct wl_buffer *buffers[2];
    unsigned pending_buffers;
    bool configured;
};

static void update_cursor(struct wayland_client *client);

#include "clipboard.inc"

static void buffer_release(void *data, struct wl_buffer *buffer)
{
    struct wayland_client *client = data;
    for (unsigned i = 0; i < 2; ++i)
        if (client->buffers[i] == buffer)
            client->buffers[i] = NULL;
    --client->pending_buffers;
    wl_buffer_destroy(buffer);
}

static void destroy_buffers(struct wayland_client *client)
{
    for (unsigned i = 0; i < 2; ++i)
        if (client->buffers[i]) {
            wl_buffer_destroy(client->buffers[i]);
            client->buffers[i] = NULL;
        }
    client->pending_buffers = 0;
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

static int redraw_delay(const struct wayland_client *client, uint64_t now)
{
    if (!client->configured || !client->app->dirty || client->pending_buffers >= 2)
        return -1;
    uint64_t deadline = client->app->redraw_at;
    if (now >= deadline)
        return 0;
    uint64_t remaining = deadline - now;
    return remaining > INT_MAX ? INT_MAX : (int)remaining;
}

static uint32_t window_buffer_format(const struct app *app)
{
    return app_corner_radius(app, app->width, app->height, 120) > 0 ? WL_SHM_FORMAT_ARGB8888
                                                                    : WL_SHM_FORMAT_XRGB8888;
}
static bool update_surface_regions(struct wayland_client *client)
{
    const struct app *app = client->app;
    int w = app->width, h = app->height;
    int r = app_corner_radius(app, w, h, 120);
    if (client->region_width == w && client->region_height == h && client->region_radius == r)
        return true;
    struct wl_region *opaque = wl_compositor_create_region(client->compositor);
    struct wl_region *input = r ? wl_compositor_create_region(client->compositor) : NULL;
    if (!opaque || (r && !input)) {
        if (opaque)
            wl_region_destroy(opaque);
        return false;
    }
    if (!r) {
        wl_region_add(opaque, 0, 0, w, h);
    } else {
        /* A conservative cross excludes all antialiased corner pixels, including
         * fractional viewport filtering. The remainder can be composed opaquely. */
        int inset = r + 1;
        if (w > 2 * inset)
            wl_region_add(opaque, inset, 0, w - 2 * inset, h);
        if (h > 2 * inset)
            wl_region_add(opaque, 0, inset, w, h - 2 * inset);
        if (h > 2 * r)
            wl_region_add(input, 0, r, w, h - 2 * r);
        for (int y = 0; y < r; ++y) {
            int x = 0, dy = r - y - 2;
            if (dy < 0)
                dy = 0;
            /* Include intersecting cells plus a one-logical-pixel fringe for
             * fractional buffer rounding/filtering and usable resize edges. */
            while ((r - x - 1) * (r - x - 1) + dy * dy >= r * r)
                ++x;
            if (x > 0)
                --x;
            wl_region_add(input, x, y, w - 2 * x, 1);
            wl_region_add(input, x, h - 1 - y, w - 2 * x, 1);
        }
    }
    wl_surface_set_opaque_region(client->surface, opaque);
    wl_surface_set_input_region(client->surface, input);
    wl_region_destroy(opaque);
    if (input)
        wl_region_destroy(input);
    client->region_width = w;
    client->region_height = h;
    client->region_radius = r;
    return true;
}

static bool draw(struct wayland_client *client)
{
    /* Coalesce edits, cap redraws, and bound outstanding compositor buffers. */
    uint64_t started = session_now();
    if (redraw_delay(client, started) != 0)
        return true;
    /* Surface dimensions are logical; buffers and font sizes use physical pixels. */
    int64_t physical_width = ((int64_t)client->app->width * client->app->scale120 + 60) / 120;
    int64_t physical_height = ((int64_t)client->app->height * client->app->scale120 + 60) / 120;
    if (physical_width < 1)
        physical_width = 1;
    if (physical_height < 1)
        physical_height = 1;
    /* wl_shm uses signed 32-bit sizes and strides. */
    if (physical_height > INT_MAX || physical_width > INT_MAX / 4 / physical_height) {
        fprintf(stderr, "Window dimensions exceed the shared-memory limit\n");
        return false;
    }
    int width = (int)physical_width, height = (int)physical_height;
    int stride = width * 4;
    int size = stride * height;
    int fd = memfd_create("mib", MFD_CLOEXEC);
    if (fd < 0) {
        perror("memfd_create");
        return false;
    }
    if (ftruncate(fd, size) < 0) {
        perror("ftruncate");
        close(fd);
        return false;
    }
    uint32_t *pixels = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return false;
    }
    bool painted = app_paint(client->app, pixels, width, height, width);
    platform_trim_heap();
    if (!painted) {
        fprintf(stderr, "Could not rasterize the bundled font\n");
        munmap(pixels, (size_t)size);
        close(fd);
        return false;
    }
    struct wl_shm_pool *pool = wl_shm_create_pool(client->shm, fd, size);
    struct wl_buffer *buffer = pool ? wl_shm_pool_create_buffer(pool, 0, width, height, stride,
                                                                window_buffer_format(client->app))
                                    : NULL;
    if (pool)
        wl_shm_pool_destroy(pool);
    /* The compositor keeps its own mapping; our pixels are never reused. */
    munmap(pixels, (size_t)size);
    close(fd);
    if (!buffer) {
        fprintf(stderr, "Could not create a Wayland shared-memory buffer\n");
        return false;
    }
    if (!update_surface_regions(client)) {
        wl_buffer_destroy(buffer);
        return false;
    }
    unsigned slot = 0;
    while (slot < 2 && client->buffers[slot])
        ++slot;
    if (slot == 2) {
        wl_buffer_destroy(buffer);
        return false;
    }
    client->buffers[slot] = buffer;
    wl_buffer_add_listener(buffer, &buffer_listener, client);
    ++client->pending_buffers;
    if (client->viewport) {
        wp_viewport_set_destination(client->viewport, client->app->width, client->app->height);
    } else if (wl_surface_get_version(client->surface) >= 3) {
        wl_surface_set_buffer_scale(client->surface, (int32_t)(client->app->scale120 / 120));
    }
    wl_surface_attach(client->surface, buffer, 0, 0);
    wl_surface_damage(client->surface, 0, 0, client->app->width, client->app->height);
    wl_surface_commit(client->surface);
    client->app->dirty = false;
    client->app->redraw_at = started + APP_FRAME_INTERVAL_MS;
    update_cursor(client);
    return true;
}

static void update_scale(struct wayland_client *client, uint32_t scale120)
{
    if (!scale120 || scale120 == client->app->scale120)
        return;
    client->app->scale120 = scale120;
    fprintf(stderr, "Render scale: %.2fx\n", scale120 / 120.0);
    client->app->dirty = true;
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
    (void)data;
    (void)surface;
    (void)output;
}

static void surface_leave(void *data, struct wl_surface *surface, struct wl_output *output)
{
    (void)data;
    (void)surface;
    (void)output;
}

static void surface_scale(void *data, struct wl_surface *surface, int32_t scale)
{
    (void)surface;
    struct wayland_client *client = data;
    if (!client->fractional_scale && scale > 0 && (uint32_t)scale <= UINT32_MAX / 120)
        update_scale(client, (uint32_t)scale * 120);
}

static void surface_transform(void *data, struct wl_surface *surface, uint32_t transform)
{
    (void)data;
    (void)surface;
    (void)transform;
}

static const struct wl_surface_listener wl_surface_listener = {
    .enter = surface_enter,
    .leave = surface_leave,
    .preferred_buffer_scale = surface_scale,
    .preferred_buffer_transform = surface_transform,
};

static void decoration_configure(void *data, struct zxdg_toplevel_decoration_v1 *decoration,
                                 uint32_t mode)
{
    (void)decoration;
    ((struct wayland_client *)data)->pending_csd =
        mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;
}
static const struct zxdg_toplevel_decoration_v1_listener decoration_listener = {
    .configure = decoration_configure,
};
static void apply_decorations(struct wayland_client *client)
{
    bool decorated = client->pending_csd && !client->fullscreen;
    if (client->app->client_decorated != decorated) {
        client->app->client_decorated = decorated;
        client->app->context_menu = false;
        client->dragging = false;
        client->app->title_pressed = TITLE_NONE;
        client->app->title_hover = TITLE_NONE;
    }
    client->app->dirty = true;
    update_cursor(client);
}

static void surface_configure(void *data, struct xdg_surface *surface, uint32_t serial)
{
    struct wayland_client *client = data;
    xdg_surface_ack_configure(surface, serial);
    apply_decorations(client);
    client->configured = true;
    /* Paint once after the current batch of protocol events. */
}

static const struct xdg_surface_listener surface_listener = {
    .configure = surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel, int32_t width,
                               int32_t height, struct wl_array *states)
{
    (void)toplevel;
    struct wayland_client *client = data;
    bool maximized = false;
    client->fullscreen = false;
    client->app->tiled = false;
    uint32_t *state;
    wl_array_for_each(state, states)
    {
        if (*state == XDG_TOPLEVEL_STATE_MAXIMIZED)
            maximized = true;
        if (*state == XDG_TOPLEVEL_STATE_FULLSCREEN)
            client->fullscreen = true;
        if (*state == XDG_TOPLEVEL_STATE_TILED_LEFT || *state == XDG_TOPLEVEL_STATE_TILED_RIGHT ||
            *state == XDG_TOPLEVEL_STATE_TILED_TOP || *state == XDG_TOPLEVEL_STATE_TILED_BOTTOM)
            client->app->tiled = true;
    }
    if (maximized && !client->app->maximized) {
        client->app->restore_width = client->app->width;
        client->app->restore_height = client->app->height;
    }
    client->app->maximized = maximized;
    if (width > 0 && width != client->app->width) {
        client->app->width = width;
        client->app->dirty = true;
    }
    if (height > 0 && height != client->app->height) {
        client->app->height = height;
        client->app->dirty = true;
    }
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
    (void)toplevel;
    struct wayland_client *client = data;
    app_request_close(client->app);
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

static void stop_repeat(struct wayland_client *client) { client->repeat_key = 0; }

static void handle_key(struct wayland_client *client, uint32_t key)
{
    if (!client->xkb_state)
        return;
    xkb_keysym_t symbol = xkb_state_key_get_one_sym(client->xkb_state, key + 8);
    enum editor_key action = KEY_TEXT;
    switch (symbol) {
    case XKB_KEY_Escape:
        action = KEY_ESCAPE;
        break;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        action = KEY_ENTER;
        break;
    case XKB_KEY_BackSpace:
        action = KEY_BACKSPACE;
        break;
    case XKB_KEY_Delete:
        action = KEY_DELETE;
        break;
    case XKB_KEY_Left:
        action = KEY_LEFT;
        break;
    case XKB_KEY_Right:
        action = KEY_RIGHT;
        break;
    case XKB_KEY_Up:
        action = KEY_UP;
        break;
    case XKB_KEY_Down:
        action = KEY_DOWN;
        break;
    case XKB_KEY_Tab:
        action = KEY_TAB;
        break;
    default:
        break;
    }
    bool control = xkb_state_mod_name_is_active(client->xkb_state, XKB_MOD_NAME_CTRL,
                                                XKB_STATE_MODS_EFFECTIVE) > 0;
    bool shift = xkb_state_mod_name_is_active(client->xkb_state, XKB_MOD_NAME_SHIFT,
                                              XKB_STATE_MODS_EFFECTIVE) > 0;
    bool alt = xkb_state_mod_name_is_active(client->xkb_state, XKB_MOD_NAME_ALT,
                                            XKB_STATE_MODS_EFFECTIVE) > 0;
    if (alt && !control) {
        /* Alt+1 to Alt+9 switch pages and Alt+0 lists them. The number row goes by position
         * (evdev codes 2 to 11) so layouts with symbols there still work; the keypad works
         * with Num Lock off too. */
        static const xkb_keysym_t keypad[][2] = {
            {XKB_KEY_KP_1, XKB_KEY_KP_End},   {XKB_KEY_KP_2, XKB_KEY_KP_Down},
            {XKB_KEY_KP_3, XKB_KEY_KP_Next},  {XKB_KEY_KP_4, XKB_KEY_KP_Left},
            {XKB_KEY_KP_5, XKB_KEY_KP_Begin}, {XKB_KEY_KP_6, XKB_KEY_KP_Right},
            {XKB_KEY_KP_7, XKB_KEY_KP_Home},  {XKB_KEY_KP_8, XKB_KEY_KP_Up},
            {XKB_KEY_KP_9, XKB_KEY_KP_Prior},
        };
        char page[2] = {0};
        if (key >= 2 && key <= 10)
            page[0] = (char)('1' + key - 2);
        else if (key == 11 || symbol == XKB_KEY_KP_0 || symbol == XKB_KEY_KP_Insert)
            page[0] = '0';
        for (unsigned i = 0; i < 9; ++i)
            if (symbol == keypad[i][0] || symbol == keypad[i][1])
                page[0] = (char)('1' + i);
        if (page[0]) {
            app_input(client->app, KEY_PAGE, page);
            return;
        }
    }
    if (symbol == XKB_KEY_ISO_Left_Tab)
        action = KEY_SHIFT_TAB;
    if (symbol == XKB_KEY_Home)
        action = KEY_HOME;
    if (symbol == XKB_KEY_End)
        action = KEY_END;
    if (symbol == XKB_KEY_Page_Up)
        action = KEY_PAGE_UP;
    if (symbol == XKB_KEY_Page_Down)
        action = KEY_PAGE_DOWN;
    if (control) {
        switch (xkb_keysym_to_lower(symbol)) {
        case XKB_KEY_bracketleft:
            action = KEY_ESCAPE;
            break;
        case XKB_KEY_b:
            action = KEY_CTRL_B;
            break;
        case XKB_KEY_c:
            action = shift ? KEY_COPY_IMAGE_TEXT : KEY_CTRL_C;
            break;
        case XKB_KEY_d:
            action = KEY_CTRL_D;
            break;
        case XKB_KEY_e:
            action = KEY_CTRL_E;
            break;
        case XKB_KEY_f:
            action = KEY_CTRL_F;
            break;
        case XKB_KEY_i:
            action = KEY_CTRL_I;
            break;
        case XKB_KEY_o:
            action = KEY_CTRL_O;
            break;
        case XKB_KEY_q:
            action = KEY_CTRL_Q;
            break;
        case XKB_KEY_r:
            action = KEY_CTRL_R;
            break;
        case XKB_KEY_s:
            action = KEY_CTRL_S;
            break;
        case XKB_KEY_u:
            action = KEY_CTRL_U;
            break;
        case XKB_KEY_v:
            action = KEY_CTRL_V;
            break;
        case XKB_KEY_w:
            action = KEY_CTRL_W;
            break;
        case XKB_KEY_y:
            action = KEY_CTRL_Y;
            break;
        case XKB_KEY_plus:
        case XKB_KEY_equal:
            action = KEY_ZOOM_IN;
            break;
        case XKB_KEY_minus:
            action = KEY_ZOOM_OUT;
            break;
        case XKB_KEY_0:
            action = KEY_ZOOM_RESET;
            break;
        default:
            return;
        }
    }
    char text[64] = {0};
    if (action == KEY_TEXT) {
        int length = xkb_state_key_get_utf8(client->xkb_state, key + 8, text, sizeof(text));
        if (length <= 0 || length >= (int)sizeof(text) || (unsigned char)text[0] < 32 ||
            (unsigned char)text[0] == 127)
            return;
    }
    app_input(client->app, action, text);
}

static void keyboard_keymap(void *data, struct wl_keyboard *keyboard, uint32_t format, int32_t fd,
                            uint32_t size)
{
    (void)keyboard;
    struct wayland_client *client = data;
    stop_repeat(client);
    struct xkb_keymap *keymap = NULL;
    if (format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && size) {
        char *mapping = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (mapping != MAP_FAILED) {
            if (mapping[size - 1] == '\0')
                keymap = xkb_keymap_new_from_string(client->xkb_context, mapping,
                                                    XKB_KEYMAP_FORMAT_TEXT_V1,
                                                    XKB_KEYMAP_COMPILE_NO_FLAGS);
            munmap(mapping, size);
        }
    }
    close(fd);
    struct xkb_state *state = keymap ? xkb_state_new(keymap) : NULL;
    if (!state) {
        xkb_keymap_unref(keymap);
        snprintf(client->app->editor.message, sizeof(client->app->editor.message),
                 "Could not load keyboard mapping");
        client->app->dirty = true;
        return;
    }
    xkb_state_unref(client->xkb_state);
    xkb_keymap_unref(client->xkb_keymap);
    client->xkb_keymap = keymap;
    client->xkb_state = state;
    platform_trim_heap();
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                           struct wl_surface *surface, struct wl_array *keys)
{
    (void)keyboard;
    (void)serial;
    (void)surface;
    (void)keys;
    session_focus(((struct wayland_client *)data)->app, true);
    stop_repeat(data);
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                           struct wl_surface *surface)
{
    (void)keyboard;
    (void)serial;
    (void)surface;
    session_focus(((struct wayland_client *)data)->app, false);
    struct wayland_client *client = data;
    stop_repeat(client);
    if (client->xkb_state)
        xkb_state_update_mask(client->xkb_state, 0, 0, 0, 0, 0, 0);
}

static int64_t now_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t time,
                         uint32_t key, uint32_t state)
{
    (void)keyboard;
    (void)time;
    ((struct wayland_client *)data)->serial = serial;
    struct wayland_client *client = data;
    if (state == WL_KEYBOARD_KEY_STATE_RELEASED) {
        if (client->repeat_key == key + 8)
            stop_repeat(client);
        return;
    }
    handle_key(client, key);
    if (client->xkb_keymap && client->repeat_rate > 0 &&
        xkb_keymap_key_repeats(client->xkb_keymap, key + 8)) {
        client->repeat_key = key + 8;
        client->repeat_at = now_ms() + client->repeat_delay;
    }
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard, uint32_t serial,
                               uint32_t depressed, uint32_t latched, uint32_t locked,
                               uint32_t group)
{
    (void)keyboard;
    (void)serial;
    struct wayland_client *client = data;
    if (client->xkb_state)
        xkb_state_update_mask(client->xkb_state, depressed, latched, locked, 0, 0, group);
}

static void keyboard_repeat(void *data, struct wl_keyboard *keyboard, int32_t rate, int32_t delay)
{
    (void)keyboard;
    struct wayland_client *client = data;
    client->repeat_rate = rate;
    client->repeat_delay = delay;
    stop_repeat(client);
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat,
};

static uint32_t resize_edge(const struct wayland_client *client, int x, int y)
{
    const struct app *app = client->app;
    if (!app->client_decorated || app->maximized || client->fullscreen)
        return XDG_TOPLEVEL_RESIZE_EDGE_NONE;
    /* Use the existing eight-pixel padding, with longer corner grab zones. */
    bool left = x < 8, right = x >= app->width - 8;
    bool top = y < 8, bottom = y >= app->height - 8;
    if (left || right) {
        top |= y < 20;
        bottom |= y >= app->height - 20;
    }
    if (top || bottom) {
        left |= x < 20;
        right |= x >= app->width - 20;
    }
    return (left    ? XDG_TOPLEVEL_RESIZE_EDGE_LEFT
            : right ? XDG_TOPLEVEL_RESIZE_EDGE_RIGHT
                    : 0) |
           (top      ? XDG_TOPLEVEL_RESIZE_EDGE_TOP
            : bottom ? XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM
                     : 0);
}
static uint32_t pointer_shape(const struct wayland_client *client)
{
    const struct app *app = client->app;
    int corner = app->image_drag ? app->drag_corner
                 : client->dragging || resize_edge(client, client->pointer_x, client->pointer_y)
                     ? -1
                     : app_image_corner(app, client->pointer_x, client->pointer_y);
    if (corner >= 0)
        return corner == 0 || corner == 3 ? WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NWSE_RESIZE
                                          : WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NESW_RESIZE;
    if (client->dragging || client->app->title_pressed != TITLE_NONE)
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
    switch (resize_edge(client, client->pointer_x, client->pointer_y)) {
    case XDG_TOPLEVEL_RESIZE_EDGE_TOP:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_N_RESIZE;
    case XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_S_RESIZE;
    case XDG_TOPLEVEL_RESIZE_EDGE_LEFT:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_W_RESIZE;
    case XDG_TOPLEVEL_RESIZE_EDGE_RIGHT:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_E_RESIZE;
    case XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NW_RESIZE;
    case XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NE_RESIZE;
    case XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SW_RESIZE;
    case XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SE_RESIZE;
    default:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
    }
}
static void update_cursor(struct wayland_client *client)
{
    enum app_title_action hover = app_title_hit(client->app, client->pointer_x, client->pointer_y);
    if (!client->cursor_serial || client->dragging ||
        resize_edge(client, client->pointer_x, client->pointer_y) || hover < TITLE_MINIMIZE)
        hover = TITLE_NONE;
    if (client->app->title_hover != hover) {
        client->app->title_hover = hover;
        client->app->dirty = true;
    }
    if (!client->cursor_device || !client->cursor_serial)
        return;
    uint32_t shape = pointer_shape(client);
    if (shape != client->cursor_shape) {
        /* Cursor requests require the enter serial, not a key/button serial. */
        wp_cursor_shape_device_v1_set_shape(client->cursor_device, client->cursor_serial, shape);
        client->cursor_shape = shape;
    }
}
static void cursor_device(struct wayland_client *client)
{
    if (client->cursor_manager && client->pointer && !client->cursor_device) {
        client->cursor_device =
            wp_cursor_shape_manager_v1_get_pointer(client->cursor_manager, client->pointer);
        update_cursor(client);
    }
}

static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
                          struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y)
{
    (void)pointer;
    (void)surface;
    struct wayland_client *c = data;
    c->serial = serial;
    c->pointer_x = wl_fixed_to_int(x);
    c->pointer_y = wl_fixed_to_int(y);
    c->cursor_serial = serial;
    c->cursor_shape = 0;
    update_cursor(c);
}
static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial,
                          struct wl_surface *surface)
{
    (void)pointer;
    (void)serial;
    (void)surface;
    struct wayland_client *client = data;
    client->dragging = false;
    app_pointer_release(client->app);
    if (client->app->title_pressed != TITLE_NONE)
        client->app->dirty = true;
    client->app->title_pressed = TITLE_NONE;
    client->cursor_serial = 0;
    client->cursor_shape = 0;
    update_cursor(client);
}
static void pointer_motion(void *data, struct wl_pointer *pointer, uint32_t time, wl_fixed_t x,
                           wl_fixed_t y)
{
    (void)pointer;
    (void)time;
    struct wayland_client *c = data;
    c->pointer_x = wl_fixed_to_int(x);
    c->pointer_y = wl_fixed_to_int(y);
    if (c->dragging)
        app_pointer(c->app, c->pointer_x, c->pointer_y, true);
    update_cursor(c);
}
static void toggle_maximized(struct wayland_client *client)
{
    if (client->app->maximized)
        xdg_toplevel_unset_maximized(client->toplevel);
    else
        xdg_toplevel_set_maximized(client->toplevel);
}
static bool decoration_button(struct wayland_client *client, uint32_t serial, uint32_t time,
                              uint32_t button, uint32_t state)
{
    enum app_title_action hit = app_title_hit(client->app, client->pointer_x, client->pointer_y);
    if (button == 0x110 && state == WL_POINTER_BUTTON_STATE_RELEASED &&
        client->app->title_pressed != TITLE_NONE) {
        enum app_title_action pressed = client->app->title_pressed;
        client->app->title_pressed = TITLE_NONE;
        client->app->dirty = true;
        if (pressed == hit) {
            if (hit == TITLE_CLOSE)
                app_request_close(client->app);
            else if (hit == TITLE_MAXIMIZE)
                toggle_maximized(client);
            else if (hit == TITLE_MINIMIZE)
                xdg_toplevel_set_minimized(client->toplevel);
        }
        return true;
    }
    if (state != WL_POINTER_BUTTON_STATE_PRESSED)
        return hit != TITLE_NONE;
    uint32_t edge = resize_edge(client, client->pointer_x, client->pointer_y);
    if (button == 0x110 && edge) {
        client->title_click = 0;
        client->dragging = false;
        xdg_toplevel_resize(client->toplevel, client->seat, serial, edge);
        return true;
    }
    if (hit == TITLE_NONE) {
        client->title_click = 0;
        return false;
    }
    client->dragging = false;
    client->last_click = 0;
    if (client->app->context_menu) {
        client->app->context_menu = false;
        client->app->dirty = true;
    }
    if (button == 0x111)
        xdg_toplevel_show_window_menu(client->toplevel, client->seat, serial, client->pointer_x,
                                      client->pointer_y);
    else if (button == 0x110) {
        if (hit == TITLE_MOVE) {
            bool twice = client->title_click && time - client->title_click < 400 &&
                         abs(client->pointer_x - client->title_click_x) < 4 &&
                         abs(client->pointer_y - client->title_click_y) < 4;
            if (twice)
                toggle_maximized(client);
            else
                xdg_toplevel_move(client->toplevel, client->seat, serial);
            client->title_click = twice ? 0 : time;
            client->title_click_x = client->pointer_x;
            client->title_click_y = client->pointer_y;
        } else {
            client->title_click = 0;
            client->app->title_pressed = hit;
            client->app->dirty = true;
        }
    }
    return true;
}

static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial, uint32_t time,
                           uint32_t button, uint32_t state)
{
    (void)pointer;
    (void)time;
    struct wayland_client *c = data;
    c->serial = serial;
    if (button == 0x110 && state == WL_POINTER_BUTTON_STATE_RELEASED) {
        c->dragging = false;
        app_pointer_release(c->app);
    }
    if (decoration_button(c, serial, time, button, state)) {
        update_cursor(c);
        return;
    }
    if (button == 0x111 && state == WL_POINTER_BUTTON_STATE_PRESSED) {
        app_context(c->app, c->pointer_x, c->pointer_y);
        return;
    }
    if (button == 0x110) {
        c->dragging = state == WL_POINTER_BUTTON_STATE_PRESSED;
        if (c->dragging) {
            bool menu = c->app->context_menu;
            app_pointer(c->app, c->pointer_x, c->pointer_y, false);
            if (menu)
                c->dragging = false;
            bool control =
                c->xkb_state && xkb_state_mod_name_is_active(c->xkb_state, XKB_MOD_NAME_CTRL,
                                                             XKB_STATE_MODS_EFFECTIVE) > 0;
            bool twice = c->last_click && time - c->last_click < 400 &&
                         abs(c->pointer_x - c->last_click_x) < 4 &&
                         abs(c->pointer_y - c->last_click_y) < 4;
            bool resizing = c->app->image_drag;
            if (!menu && !resizing && (control || twice)) {
                linux_service(c->app, ED_OPEN, c);
                c->dragging = false;
            }
            c->last_click = twice || resizing ? 0 : time;
            c->last_click_x = c->pointer_x;
            c->last_click_y = c->pointer_y;
        }
        update_cursor(c);
    }
}
static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis,
                         wl_fixed_t value)
{
    (void)pointer;
    (void)time;
    struct wayland_client *client = data;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL &&
        app_title_hit(client->app, client->pointer_x, client->pointer_y) == TITLE_NONE)
        app_scroll(client->app, value > 0 ? 3 : -3);
}
static const struct wl_pointer_listener pointer_listener = {.enter = pointer_enter,
                                                            .leave = pointer_leave,
                                                            .motion = pointer_motion,
                                                            .button = pointer_button,
                                                            .axis = pointer_axis};
static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t capabilities)
{
    struct wayland_client *client = data;
    if ((capabilities & WL_SEAT_CAPABILITY_POINTER) && !client->pointer) {
        client->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(client->pointer, &pointer_listener, client);
        cursor_device(client);
    } else if (!(capabilities & WL_SEAT_CAPABILITY_POINTER) && client->pointer) {
        pointer_leave(client, client->pointer, 0, NULL);
        if (client->cursor_device)
            wp_cursor_shape_device_v1_destroy(client->cursor_device);
        client->cursor_device = NULL;
        client->cursor_serial = client->cursor_shape = 0;
        wl_pointer_destroy(client->pointer);
        client->pointer = NULL;
    }
    if ((capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && !client->keyboard) {
        client->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(client->keyboard, &keyboard_listener, client);
    } else if (!(capabilities & WL_SEAT_CAPABILITY_KEYBOARD) && client->keyboard) {
        wl_keyboard_release(client->keyboard);
        client->keyboard = NULL;
        stop_repeat(client);
    }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name)
{
    (void)data;
    (void)seat;
    (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
                            const char *interface, uint32_t version)
{
    struct wayland_client *client = data;
    if (strcmp(interface, wl_compositor_interface.name) == 0)
        client->compositor =
            wl_registry_bind(registry, name, &wl_compositor_interface, version < 6 ? version : 6);
    else if (strcmp(interface, wl_seat_interface.name) == 0 && !client->seat) {
        client->seat =
            wl_registry_bind(registry, name, &wl_seat_interface, version < 4 ? version : 4);
        wl_seat_add_listener(client->seat, &seat_listener, client);
    } else if (strcmp(interface, wl_data_device_manager_interface.name) == 0)
        client->data_manager = wl_registry_bind(registry, name, &wl_data_device_manager_interface,
                                                version < 3 ? version : 3);
    else if (strcmp(interface, wl_shm_interface.name) == 0)
        client->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    else if (strcmp(interface, wp_viewporter_interface.name) == 0)
        client->viewporter = wl_registry_bind(registry, name, &wp_viewporter_interface, 1);
    else if (strcmp(interface, wp_fractional_scale_manager_v1_interface.name) == 0)
        client->scale_manager =
            wl_registry_bind(registry, name, &wp_fractional_scale_manager_v1_interface, 1);
    else if (strcmp(interface, wp_cursor_shape_manager_v1_interface.name) == 0) {
        client->cursor_manager =
            wl_registry_bind(registry, name, &wp_cursor_shape_manager_v1_interface, 1);
        cursor_device(client);
    } else if (strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0)
        client->decoration_manager =
            wl_registry_bind(registry, name, &zxdg_decoration_manager_v1_interface, 1);
    else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        /* Version 2 adds tiled-edge states; keep compatibility with version 1. */
        client->wm =
            wl_registry_bind(registry, name, &xdg_wm_base_interface, version < 2 ? version : 2);
        xdg_wm_base_add_listener(client->wm, &wm_listener, client);
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

void platform_report(const char *text, bool error)
{
    fprintf(error ? stderr : stdout, "%s\n", text);
}
int platform_run(struct app *app)
{
    struct wayland_client state = {.app = app, .pending_csd = true, .receive_fd = -1};
    app->service = linux_service;
    app->service_data = &state;
    signal(SIGPIPE, SIG_IGN);
    struct wayland_client *client = &state;
    int status = EXIT_FAILURE;
    client->xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!client->xkb_context)
        goto cleanup;
    client->display = wl_display_connect(NULL);
    if (!client->display) {
        fprintf(stderr, "Cannot connect to Wayland: %s\n", strerror(errno));
        goto cleanup;
    }
    client->registry = wl_display_get_registry(client->display);
    wl_registry_add_listener(client->registry, &registry_listener, client);
    if (wl_display_roundtrip(client->display) < 0)
        goto cleanup;
    if (!client->compositor || !client->shm || !client->wm) {
        fprintf(stderr, "Compositor must provide wl_compositor, wl_shm, and xdg_wm_base\n");
        goto cleanup;
    }
    if (client->seat && client->data_manager) {
        client->data_device =
            wl_data_device_manager_get_data_device(client->data_manager, client->seat);
        wl_data_device_add_listener(client->data_device, &data_listener, client);
    }
    client->surface = wl_compositor_create_surface(client->compositor);
    wl_surface_add_listener(client->surface, &wl_surface_listener, client);
    if (client->viewporter && client->scale_manager) {
        client->viewport = wp_viewporter_get_viewport(client->viewporter, client->surface);
        client->fractional_scale = wp_fractional_scale_manager_v1_get_fractional_scale(
            client->scale_manager, client->surface);
        wp_fractional_scale_v1_add_listener(client->fractional_scale, &scale_listener, client);
    }
    client->xdg_surface = xdg_wm_base_get_xdg_surface(client->wm, client->surface);
    xdg_surface_add_listener(client->xdg_surface, &surface_listener, client);
    client->toplevel = xdg_surface_get_toplevel(client->xdg_surface);
    xdg_toplevel_add_listener(client->toplevel, &toplevel_listener, client);
    if (client->decoration_manager) {
        client->decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(
            client->decoration_manager, client->toplevel);
        zxdg_toplevel_decoration_v1_add_listener(client->decoration, &decoration_listener, client);
        zxdg_toplevel_decoration_v1_set_mode(client->decoration,
                                             ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }
    xdg_toplevel_set_title(client->toplevel, "mib");
    xdg_toplevel_set_app_id(client->toplevel, "io.github.osigurdson.mib");
    if (app->maximized)
        xdg_toplevel_set_maximized(client->toplevel);

    /* Initial empty commit asks the compositor to configure the window. */
    wl_surface_commit(client->surface);
    while (client->app->running) {
        app_tick(client->app);
        if (wl_display_dispatch_pending(client->display) < 0) {
            client->app->failed = true;
            break;
        }
        if (!client->app->running)
            break;
        if (client->repeat_key && now_ms() >= client->repeat_at) {
            handle_key(client, client->repeat_key - 8);
            int interval = 1000 / client->repeat_rate;
            client->repeat_at = now_ms() + (interval > 0 ? interval : 1);
        }
        if (!client->app->running)
            break;
        if (client->configured && !draw(client)) {
            client->app->failed = true;
            break;
        }
        int flush = wl_display_flush(client->display);
        if (flush < 0 && errno != EAGAIN) {
            client->app->failed = true;
            break;
        }
        struct pollfd socket = {.fd = wl_display_get_fd(client->display),
                                .events = POLLIN | (flush < 0 ? POLLOUT : 0)};
        int timeout = -1;
        if (client->repeat_key) {
            int64_t remaining = client->repeat_at - now_ms();
            timeout = remaining <= 0 ? 0 : remaining > INT_MAX ? INT_MAX : (int)remaining;
        }
        int redraw = redraw_delay(client, session_now());
        if (redraw >= 0 && (timeout < 0 || redraw < timeout))
            timeout = redraw;
        uint64_t deadline = app_deadline(app);
        if (deadline) {
            uint64_t now = session_now();
            int remaining = deadline <= now ? 0 : (int)(deadline - now);
            if (timeout < 0 || remaining < timeout)
                timeout = remaining;
        }
        struct pollfd sockets[20] = {socket, {.fd = client->receive_fd, .events = POLLIN}};
        struct clipboard_source *send_sources[16];
        unsigned send_slots[16], count = 2;
        for (struct clipboard_source *p = client->sources; p; p = p->next)
            for (unsigned i = 0; i < 4; ++i)
                if (p->sends[i].fd >= 0) {
                    send_sources[count - 2] = p;
                    send_slots[count - 2] = i;
                    sockets[count++] = (struct pollfd){.fd = p->sends[i].fd, .events = POLLOUT};
                }
        int ready = poll(sockets, count, timeout);
        for (unsigned i = 2; i < count; ++i)
            if (sockets[i].revents)
                send_ready(send_sources[i - 2], send_slots[i - 2]);
        if (sockets[1].revents)
            receive_ready(client);
        socket = sockets[0];
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0 || (socket.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            client->app->failed = true;
            break;
        }
        if ((socket.revents & POLLIN) && wl_display_dispatch(client->display) < 0) {
            client->app->failed = true;
            break;
        }
    }
    status = client->app->failed ? EXIT_FAILURE : EXIT_SUCCESS;

cleanup:
    destroy_buffers(client);
    receive_end(client, false);
    offer_unref(client->pending_offer);
    offer_unref(client->selection_offer);
    offer_unref(client->drag_offer);
    while (client->sources)
        source_free(client->sources);
    if (client->data_device)
        wl_data_device_destroy(client->data_device);
    if (client->data_manager)
        wl_data_device_manager_destroy(client->data_manager);
    app->service = NULL;
    app->service_data = NULL;
    if (client->cursor_device)
        wp_cursor_shape_device_v1_destroy(client->cursor_device);
    if (client->cursor_manager)
        wp_cursor_shape_manager_v1_destroy(client->cursor_manager);
    if (client->pointer)
        wl_pointer_destroy(client->pointer);
    if (client->keyboard)
        wl_keyboard_destroy(client->keyboard);
    if (client->seat)
        wl_seat_destroy(client->seat);
    xkb_state_unref(client->xkb_state);
    xkb_keymap_unref(client->xkb_keymap);
    xkb_context_unref(client->xkb_context);
    if (client->decoration)
        zxdg_toplevel_decoration_v1_destroy(client->decoration);
    if (client->toplevel)
        xdg_toplevel_destroy(client->toplevel);
    if (client->decoration_manager)
        zxdg_decoration_manager_v1_destroy(client->decoration_manager);
    if (client->xdg_surface)
        xdg_surface_destroy(client->xdg_surface);
    if (client->fractional_scale)
        wp_fractional_scale_v1_destroy(client->fractional_scale);
    if (client->viewport)
        wp_viewport_destroy(client->viewport);
    if (client->surface)
        wl_surface_destroy(client->surface);
    if (client->scale_manager)
        wp_fractional_scale_manager_v1_destroy(client->scale_manager);
    if (client->viewporter)
        wp_viewporter_destroy(client->viewporter);
    if (client->wm)
        xdg_wm_base_destroy(client->wm);
    if (client->shm)
        wl_shm_destroy(client->shm);
    if (client->compositor)
        wl_compositor_destroy(client->compositor);
    if (client->registry)
        wl_registry_destroy(client->registry);
    if (client->display)
        wl_display_disconnect(client->display);
    return status;
}
