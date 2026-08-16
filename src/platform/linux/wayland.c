#define _GNU_SOURCE
#include "app.h"
#include "media.h"
#include "fractional-scale-v1-client-protocol.h"
#include "platform.h"
#include "session.h"
#include "viewporter-client-protocol.h"
#include "xdg-activation-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"
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
    struct wp_viewporter *viewporter;
    struct wp_viewport *viewport;
    struct wp_fractional_scale_manager_v1 *scale_manager;
    struct wp_fractional_scale_v1 *fractional_scale;
    struct xdg_activation_v1 *activation;
    struct wl_seat *seat;
    struct wl_keyboard *keyboard;
    struct wl_pointer *pointer;
    int pointer_x, pointer_y;
    bool dragging;
    uint32_t serial, last_click;
    int last_click_x, last_click_y;
    struct wl_data_device_manager *data_manager;
    struct wl_data_device *data_device;
    struct clipboard_offer *pending_offer, *selection_offer, *drag_offer, *receive_offer;
    struct clipboard_source *sources;
    int receive_fd, receive_kind;
    char *receive_data;
    size_t receive_length, receive_capacity;
    struct xkb_context *xkb_context;
    struct xkb_keymap *xkb_keymap;
    struct xkb_state *xkb_state;
    uint32_t repeat_key;
    int repeat_rate, repeat_delay;
    int64_t repeat_at;
    unsigned pending_buffers;
    bool configured;
};

#include "clipboard.inc"

static void buffer_release(void *data, struct wl_buffer *buffer)
{
    struct wayland_client *client = data;
    --client->pending_buffers;
    wl_buffer_destroy(buffer);
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

static bool draw(struct wayland_client *client)
{
    /* Coalesce edits and bound memory while the compositor reads old frames. */
    if (!client->app->dirty || client->pending_buffers >= 2)
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
    struct wl_buffer *buffer =
        pool ? wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_XRGB8888)
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

static void surface_configure(void *data, struct xdg_surface *surface, uint32_t serial)
{
    struct wayland_client *client = data;
    xdg_surface_ack_configure(surface, serial);
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
    uint32_t *state;
    wl_array_for_each(state, states) if (*state == XDG_TOPLEVEL_STATE_MAXIMIZED) maximized = true;
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

static void pointer_enter(void *data, struct wl_pointer *pointer, uint32_t serial,
                          struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y)
{
    (void)pointer;
    (void)surface;
    struct wayland_client *c = data;
    c->serial = serial;
    c->pointer_x = wl_fixed_to_int(x);
    c->pointer_y = wl_fixed_to_int(y);
}
static void pointer_leave(void *data, struct wl_pointer *pointer, uint32_t serial,
                          struct wl_surface *surface)
{
    (void)pointer;
    (void)serial;
    (void)surface;
    ((struct wayland_client *)data)->dragging = false;
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
}
static void pointer_button(void *data, struct wl_pointer *pointer, uint32_t serial, uint32_t time,
                           uint32_t button, uint32_t state)
{
    (void)pointer;
    (void)time;
    struct wayland_client *c = data;
    c->serial = serial;
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
            if (!menu && (control || twice)) {
                linux_service(c->app, ED_OPEN, c);
                c->dragging = false;
            }
            c->last_click = twice ? 0 : time;
            c->last_click_x = c->pointer_x;
            c->last_click_y = c->pointer_y;
        }
    }
}
static void pointer_axis(void *data, struct wl_pointer *pointer, uint32_t time, uint32_t axis,
                         wl_fixed_t value)
{
    (void)pointer;
    (void)time;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
        app_scroll(((struct wayland_client *)data)->app, value > 0 ? 3 : -3);
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
    } else if (!(capabilities & WL_SEAT_CAPABILITY_POINTER) && client->pointer) {
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
    } else if (strcmp(interface, xdg_activation_v1_interface.name) == 0)
        client->activation = wl_registry_bind(registry, name, &xdg_activation_v1_interface, 1);
    else if (strcmp(interface, wl_data_device_manager_interface.name) == 0)
        client->data_manager = wl_registry_bind(registry, name, &wl_data_device_manager_interface,
                                                version < 3 ? version : 3);
    else if (strcmp(interface, wl_shm_interface.name) == 0)
        client->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    else if (strcmp(interface, wp_viewporter_interface.name) == 0)
        client->viewporter = wl_registry_bind(registry, name, &wp_viewporter_interface, 1);
    else if (strcmp(interface, wp_fractional_scale_manager_v1_interface.name) == 0)
        client->scale_manager =
            wl_registry_bind(registry, name, &wp_fractional_scale_manager_v1_interface, 1);
    else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        client->wm = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
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

int platform_run(struct app *app)
{
    struct wayland_client state = {.app = app, .receive_fd = -1};
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
    xdg_toplevel_set_title(client->toplevel, "Tiny vi — CPU / Wayland");
    xdg_toplevel_set_app_id(client->toplevel, "tiny-vi");
    if (app->maximized)
        xdg_toplevel_set_maximized(client->toplevel);

    /* Initial empty commit asks the compositor to configure the window. */
    wl_surface_commit(client->surface);
    while (client->app->running) {
        media_tick(client->app);
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
        if (app->work_at) {
            uint64_t now = session_now();
            int remaining = app->work_at <= now ? 0 : (int)(app->work_at - now);
            if (timeout < 0 || remaining < timeout)
                timeout = remaining;
        }
        struct pollfd sockets[20] = {socket,
                                     {.fd = app->activation_fd, .events = POLLIN},
                                     {.fd = client->receive_fd, .events = POLLIN}};
        struct clipboard_source *send_sources[16];
        unsigned send_slots[16], count = 3;
        for (struct clipboard_source *p = client->sources; p; p = p->next)
            for (unsigned i = 0; i < 4; ++i)
                if (p->sends[i].fd >= 0) {
                    send_sources[count - 3] = p;
                    send_slots[count - 3] = i;
                    sockets[count++] = (struct pollfd){.fd = p->sends[i].fd, .events = POLLOUT};
                }
        int ready = poll(sockets, count, timeout);
        for (unsigned i = 3; i < count; ++i)
            if (sockets[i].revents)
                send_ready(send_sources[i - 3], send_slots[i - 3]);
        if (sockets[2].revents)
            receive_ready(client);
        socket = sockets[0];
        if (sockets[1].revents & POLLIN) {
            char token[1024];
            ssize_t n = recv(app->activation_fd, token, sizeof(token) - 1, 0);
            if (n >= 0) {
                token[n] = 0;
                if (client->activation && *token)
                    xdg_activation_v1_activate(client->activation, token, client->surface);
                app->dirty = true;
            }
        }
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
    if (client->activation)
        xdg_activation_v1_destroy(client->activation);
    if (client->pointer)
        wl_pointer_destroy(client->pointer);
    if (client->keyboard)
        wl_keyboard_destroy(client->keyboard);
    if (client->seat)
        wl_seat_destroy(client->seat);
    xkb_state_unref(client->xkb_state);
    xkb_keymap_unref(client->xkb_keymap);
    xkb_context_unref(client->xkb_context);
    if (client->toplevel)
        xdg_toplevel_destroy(client->toplevel);
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
