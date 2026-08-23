/* Wayland data-device text/PNG/URI clipboard and drag/drop. Transfers are
   nonblocking, bounded, and polled with the display; no helper process. */
#include "media.h"
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
struct clipboard_offer {
    struct wl_data_offer *proxy;
    unsigned refs;
    bool text, plain, png, uris, drag, fragment;
};
struct clipboard_send {
    int fd;
    size_t offset, length;
    const char *data;
};
struct clipboard_source {
    struct clipboard_source *next;
    struct wayland_client *client;
    struct wl_data_source *proxy;
    char *data, *fragment; /* data is PNG bytes when png, otherwise UTF-8 text */
    size_t length, fragment_length;
    bool png;
    struct clipboard_send sends[4];
};
static void offer_unref(struct clipboard_offer *offer)
{
    if (offer && !--offer->refs) {
        wl_data_offer_destroy(offer->proxy);
        free(offer);
    }
}
static const char *offer_mime(struct clipboard_offer *offer)
{
    return !offer            ? NULL
           : offer->png      ? "image/png"
           : offer->fragment ? "application/x-jot-fragment"
           : offer->uris     ? "text/uri-list"
           : offer->text     ? "text/plain;charset=utf-8"
           : offer->plain    ? "text/plain"
                             : NULL;
}
static void offer_type(void *data, struct wl_data_offer *proxy, const char *mime)
{
    (void)proxy;
    struct clipboard_offer *offer = data;
    if (!strcmp(mime, "image/png"))
        offer->png = true;
    if (!strcmp(mime, "application/x-jot-fragment"))
        offer->fragment = true;
    if (!strcmp(mime, "text/uri-list"))
        offer->uris = true;
    if (!strcmp(mime, "text/plain;charset=utf-8"))
        offer->text = true;
    if (!strcmp(mime, "text/plain"))
        offer->plain = true;
}
static void offer_actions(void *data, struct wl_data_offer *offer, uint32_t actions)
{
    (void)data;
    (void)offer;
    (void)actions;
}
static const struct wl_data_offer_listener offer_listener = {
    .offer = offer_type, .source_actions = offer_actions, .action = offer_actions};
static void data_offer(void *data, struct wl_data_device *device, struct wl_data_offer *proxy)
{
    (void)device;
    struct wayland_client *c = data;
    struct clipboard_offer *offer = calloc(1, sizeof(*offer));
    if (!offer) {
        wl_data_offer_destroy(proxy);
        return;
    }
    offer->proxy = proxy;
    offer->refs = 1;
    offer_unref(c->pending_offer);
    c->pending_offer = offer;
    wl_data_offer_add_listener(proxy, &offer_listener, offer);
}
static void selection(void *data, struct wl_data_device *device, struct wl_data_offer *proxy)
{
    (void)device;
    struct wayland_client *c = data;
    offer_unref(c->selection_offer);
    c->selection_offer = proxy ? wl_data_offer_get_user_data(proxy) : NULL;
    if (c->selection_offer == c->pending_offer)
        c->pending_offer = NULL;
}
static void receive_end(struct wayland_client *c, bool apply)
{
    if (c->receive_fd >= 0)
        close(c->receive_fd);
    c->receive_fd = -1;
    if (apply && c->receive_length) {
        if (c->receive_kind == 3)
            media_paste_fragment(c->app, (unsigned char *)c->receive_data, c->receive_length);
        else if (c->receive_kind == 1)
            media_paste_image(c->app, (unsigned char *)c->receive_data, c->receive_length);
        else if (c->receive_kind == 2)
            media_import_uris(c->app, c->receive_data, c->receive_length);
        else
            media_paste_text(c->app, c->receive_data, c->receive_length);
    }
    if (c->receive_offer && c->receive_offer->drag &&
        wl_data_offer_get_version(c->receive_offer->proxy) >= 3)
        wl_data_offer_finish(c->receive_offer->proxy);
    offer_unref(c->receive_offer);
    c->receive_offer = NULL;
    free(c->receive_data);
    c->receive_data = NULL;
    c->receive_length = c->receive_capacity = 0;
}
static bool receive_start(struct wayland_client *c, struct clipboard_offer *offer)
{
    const char *mime = offer_mime(offer);
    if (!mime)
        return false;
    receive_end(c, false);
    int pipes[2];
    if (pipe2(pipes, O_CLOEXEC | O_NONBLOCK) != 0)
        return false;
    /* The sending application chooses blocking/nonblocking behavior for its end. */
    fcntl(pipes[1], F_SETFL, fcntl(pipes[1], F_GETFL) & ~O_NONBLOCK);
    wl_data_offer_receive(offer->proxy, mime, pipes[1]);
    close(pipes[1]);
    c->receive_fd = pipes[0];
    c->receive_offer = offer;
    ++offer->refs;
    c->receive_kind = offer->png ? 1 : offer->fragment ? 3 : offer->uris ? 2 : 0;
    return true;
}
static void receive_ready(struct wayland_client *c)
{
    for (;;) {
        if (c->receive_length == c->receive_capacity) {
            size_t next = c->receive_capacity ? c->receive_capacity * 2 : 65536;
            if (next > 32 * 1024 * 1024) {
                snprintf(c->app->editor.message, sizeof(c->app->editor.message),
                         "Clipboard exceeds 32 MiB limit");
                receive_end(c, false);
                c->app->dirty = true;
                return;
            }
            char *p = realloc(c->receive_data, next);
            if (!p) {
                receive_end(c, false);
                return;
            }
            c->receive_data = p;
            c->receive_capacity = next;
        }
        ssize_t n = read(c->receive_fd, c->receive_data + c->receive_length,
                         c->receive_capacity - c->receive_length);
        if (n > 0)
            c->receive_length += (size_t)n;
        else if (!n) {
            receive_end(c, true);
            return;
        } else if (errno == EINTR)
            continue;
        else if (errno == EAGAIN)
            return;
        else {
            receive_end(c, false);
            return;
        }
    }
}
static void drag_enter(void *data, struct wl_data_device *device, uint32_t serial,
                       struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y,
                       struct wl_data_offer *proxy)
{
    (void)device;
    (void)surface;
    struct wayland_client *c = data;
    c->pointer_x = wl_fixed_to_int(x);
    c->pointer_y = wl_fixed_to_int(y);
    offer_unref(c->drag_offer);
    c->drag_offer = proxy ? wl_data_offer_get_user_data(proxy) : NULL;
    if (c->drag_offer == c->pending_offer)
        c->pending_offer = NULL;
    if (c->drag_offer) {
        c->drag_offer->drag = true;
        if (wl_data_offer_get_version(proxy) >= 3)
            wl_data_offer_set_actions(proxy, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY,
                                      WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
        wl_data_offer_accept(proxy, serial, offer_mime(c->drag_offer));
    }
}
static void drag_leave(void *data, struct wl_data_device *device)
{
    (void)device;
    struct wayland_client *c = data;
    offer_unref(c->drag_offer);
    c->drag_offer = NULL;
}
static void drag_motion(void *data, struct wl_data_device *device, uint32_t time, wl_fixed_t x,
                        wl_fixed_t y)
{
    (void)device;
    (void)time;
    struct wayland_client *c = data;
    c->pointer_x = wl_fixed_to_int(x);
    c->pointer_y = wl_fixed_to_int(y);
}
static void drag_drop(void *data, struct wl_data_device *device)
{
    (void)device;
    struct wayland_client *c = data;
    app_pointer(c->app, c->pointer_x, c->pointer_y, false);
    receive_start(c, c->drag_offer);
}
static const struct wl_data_device_listener data_listener = {.data_offer = data_offer,
                                                             .enter = drag_enter,
                                                             .leave = drag_leave,
                                                             .motion = drag_motion,
                                                             .drop = drag_drop,
                                                             .selection = selection};
static void source_free(struct clipboard_source *source)
{
    struct clipboard_source **p = &source->client->sources;
    while (*p && *p != source)
        p = &(*p)->next;
    if (*p)
        *p = source->next;
    for (unsigned i = 0; i < 4; ++i)
        if (source->sends[i].fd >= 0)
            close(source->sends[i].fd);
    wl_data_source_destroy(source->proxy);
    free(source->data);
    free(source->fragment);
    free(source);
}
static void source_target(void *data, struct wl_data_source *source, const char *mime)
{
    (void)data;
    (void)source;
    (void)mime;
}
static void source_cancel(void *data, struct wl_data_source *source)
{
    (void)source;
    source_free(data);
}
static void source_send(void *data, struct wl_data_source *proxy, const char *mime, int32_t fd)
{
    (void)proxy;
    struct clipboard_source *source = data;
    const char *bytes = NULL;
    size_t length = 0;
    if (!strcmp(mime, "image/png") && source->png) {
        bytes = source->data;
        length = source->length;
    } else if (!strcmp(mime, "application/x-jot-fragment")) {
        bytes = source->fragment;
        length = source->fragment_length;
    } else if ((!strcmp(mime, "text/plain;charset=utf-8") || !strcmp(mime, "text/plain")) &&
               !source->png) {
        bytes = source->data;
        length = source->length;
    }
    if (!bytes) {
        close(fd);
        return;
    }
    for (unsigned i = 0; i < 4; ++i)
        if (source->sends[i].fd < 0) {
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
            source->sends[i] =
                (struct clipboard_send){.fd = fd, .offset = 0, .length = length, .data = bytes};
            return;
        }
    close(fd);
}
static const struct wl_data_source_listener source_listener = {
    .target = source_target, .send = source_send, .cancelled = source_cancel};
static void send_ready(struct clipboard_source *s, unsigned i)
{
    struct clipboard_send *send = &s->sends[i];
    while (send->offset < send->length) {
        ssize_t n = write(send->fd, send->data + send->offset, send->length - send->offset);
        if (n > 0)
            send->offset += (size_t)n;
        else if (n < 0 && errno == EINTR)
            continue;
        else if (n < 0 && errno == EAGAIN)
            return;
        else
            break;
    }
    close(send->fd);
    send->fd = -1;
}
static void linux_open(const char *target)
{
    if (!target)
        return;
    pid_t child = fork();
    if (child == 0) {
        pid_t grandchild = fork();
        if (grandchild < 0)
            _exit(1);
        if (grandchild)
            _exit(0);
        setsid();
        execlp("xdg-open", "xdg-open", target, (char *)NULL);
        _exit(127);
    }
    if (child > 0)
        while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {
        }
}
static void linux_service(struct app *app, unsigned requests, void *context)
{
    struct wayland_client *c = context;
    struct editor *ed = &app->editor;
    if (requests & (ED_OPEN | ED_OPEN_DIRECTORY)) {
        char *path = requests & ED_OPEN_DIRECTORY ? (app->directory ? session_path(app, "") : NULL)
                                                  : media_target(app);
        linux_open(path);
        free(path);
    }
    if (requests & (ED_COPY | ED_COPY_OCR)) {
        char *data = NULL;
        size_t n = 0;
        bool png = false;
        const char *image_path = requests & ED_COPY_OCR ? NULL : app->clipboard_image;
        if (image_path) {
            data = media_clipboard_image(app, &n);
            png = data != NULL;
        } else if (requests & ED_COPY_OCR)
            data = media_ocr_text(app, &n);
        else if (ed->register_text) {
            n = ed->register_length;
            data = malloc(n + 1);
            if (data)
                memcpy(data, ed->register_text, n + 1);
        }
        if (data && c->data_manager && c->data_device && c->serial) {
            unsigned count = 0;
            for (struct clipboard_source *p = c->sources; p; p = p->next)
                ++count;
            if (count >= 4) {
                struct clipboard_source *p = c->sources;
                while (p->next)
                    p = p->next;
                source_free(p);
            }
            struct clipboard_source *s = calloc(1, sizeof(*s));
            if (s) {
                s->client = c;
                s->data = data;
                s->length = n;
                s->png = png;
                if (!(requests & ED_COPY_OCR))
                    s->fragment = media_fragment(app, &s->fragment_length);
                for (unsigned i = 0; i < 4; ++i)
                    s->sends[i].fd = -1;
                s->proxy = wl_data_device_manager_create_data_source(c->data_manager);
                s->next = c->sources;
                c->sources = s;
                wl_data_source_add_listener(s->proxy, &source_listener, s);
                /* An image is offered only as an image: apps that are offered text as well,
                 * such as browsers, paste the text. Jot itself uses the fragment. */
                if (png) {
                    wl_data_source_offer(s->proxy, "image/png");
                } else {
                    wl_data_source_offer(s->proxy, "text/plain;charset=utf-8");
                    wl_data_source_offer(s->proxy, "text/plain");
                }
                if (s->fragment)
                    wl_data_source_offer(s->proxy, "application/x-jot-fragment");
                wl_data_device_set_selection(c->data_device, s->proxy, c->serial);
                data = NULL;
            }
        }
        free(data);
    }
    if (requests & ED_PASTE) {
        if (!receive_start(c, c->selection_offer))
            editor_put(ed, ed->register_text, ed->register_length, ed->register_linewise,
                       ed->put_after, ed->put_count);
    }
}
