/* Exercise native Wayland clipboard against a private protocol server. This
   never connects to the user's display or changes their clipboard. */
#include "../src/platform/linux/wayland.c"
#include <assert.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <wayland-server-protocol.h>
#include <wayland-server.h>
struct test_source {
    bool png, fragment, text;
};
struct test_server {
    struct wl_display *display;
    struct wl_resource *device, *source, *surface;
    struct wl_event_source *reader;
    int read_fd, result_fd;
    size_t received;
    const unsigned char *png;
    size_t png_length;
    char uri[2048];
    bool reading_png, copied_text, copied_png, finished_drop;
};
static struct test_server server;
static void destroy_resource(struct wl_client *c, struct wl_resource *r)
{
    (void)c;
    wl_resource_destroy(r);
}
static void source_destroyed(struct wl_resource *r) { free(wl_resource_get_user_data(r)); }
static void offered(struct wl_client *c, struct wl_resource *r, const char *mime)
{
    (void)c;
    struct test_source *s = wl_resource_get_user_data(r);
    if (!strcmp(mime, "image/png"))
        s->png = true;
    if (!strcmp(mime, "application/x-mib-fragment"))
        s->fragment = true;
    if (!strcmp(mime, "text/plain;charset=utf-8"))
        s->text = true;
}
static void source_action(struct wl_client *c, struct wl_resource *r, uint32_t action)
{
    (void)c;
    (void)r;
    (void)action;
}
static const struct wl_data_source_interface source_impl = {
    .offer = offered, .destroy = destroy_resource, .set_actions = source_action};
static void new_source(struct wl_client *c, struct wl_resource *manager, uint32_t id)
{
    struct wl_resource *r =
        wl_resource_create(c, &wl_data_source_interface, wl_resource_get_version(manager), id);
    assert(r);
    struct test_source *s = calloc(1, sizeof(*s));
    assert(s);
    wl_resource_set_implementation(r, &source_impl, s, source_destroyed);
}
static void offer_accept(struct wl_client *c, struct wl_resource *r, uint32_t serial,
                         const char *mime)
{
    (void)c;
    (void)r;
    (void)serial;
    (void)mime;
}
static void offer_receive(struct wl_client *c, struct wl_resource *r, const char *mime, int32_t fd)
{
    (void)c;
    bool uris = wl_resource_get_user_data(r) != NULL;
    const char *text = uris ? server.uri : "pasted from fake Wayland\n";
    assert(!strcmp(mime, uris ? "text/uri-list" : "text/plain"));
    size_t n = strlen(text);
    assert(write(fd, text, n) == (ssize_t)n);
    close(fd);
}
static void offer_finish(struct wl_client *c, struct wl_resource *r)
{
    (void)c;
    (void)r;
    server.finished_drop = true;
    assert(write(server.result_fd, "F", 1) == 1);
}
static void offer_set_actions(struct wl_client *c, struct wl_resource *r, uint32_t actions,
                              uint32_t preferred)
{
    (void)c;
    (void)r;
    assert(actions == WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY && preferred == actions);
}
static const struct wl_data_offer_interface offer_impl = {.accept = offer_accept,
                                                          .receive = offer_receive,
                                                          .destroy = destroy_resource,
                                                          .finish = offer_finish,
                                                          .set_actions = offer_set_actions};
static void surface_commit(struct wl_client *c, struct wl_resource *r)
{
    (void)c;
    (void)r;
}
static const struct wl_surface_interface surface_impl = {.destroy = destroy_resource,
                                                         .commit = surface_commit};
static void new_surface(struct wl_client *c, struct wl_resource *compositor, uint32_t id)
{
    (void)compositor;
    server.surface = wl_resource_create(c, &wl_surface_interface, 1, id);
    assert(server.surface);
    wl_resource_set_implementation(server.surface, &surface_impl, NULL, NULL);
}
static const struct wl_compositor_interface compositor_impl = {.create_surface = new_surface};
static void bind_compositor(struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &wl_compositor_interface, (int)version, id);
    assert(r);
    wl_resource_set_implementation(r, &compositor_impl, NULL, NULL);
}
static void external_offer(bool drag)
{
    struct wl_client *c = wl_resource_get_client(server.device);
    struct wl_resource *r = wl_resource_create(c, &wl_data_offer_interface, 3, 0);
    assert(r);
    wl_resource_set_implementation(r, &offer_impl, drag ? &server : NULL, NULL);
    wl_data_device_send_data_offer(server.device, r);
    wl_data_offer_send_offer(r, drag ? "text/uri-list" : "text/plain");
    /* Any app can add a uri-list to what it copies, such as file:///home/me/.ssh/id_ed25519
     * from a web page; without a file manager's copied-files type, the text is pasted. */
    if (!drag)
        wl_data_offer_send_offer(r, "text/uri-list");
    if (drag) {
        wl_data_offer_send_source_actions(r, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
        wl_data_device_send_enter(server.device, 7, server.surface, wl_fixed_from_int(50),
                                  wl_fixed_from_int(50), r);
        wl_data_device_send_drop(server.device);
        wl_data_device_send_leave(server.device);
    } else
        wl_data_device_send_selection(server.device, r);
    wl_client_flush(c);
}
static int source_read(int fd, uint32_t mask, void *data)
{
    (void)mask;
    (void)data;
    unsigned char block[8192];
    for (;;) {
        ssize_t n = read(fd, block, sizeof(block));
        if (n < 0 && errno == EAGAIN)
            return 0;
        if (n < 0 && errno == EINTR)
            continue;
        assert(n >= 0);
        if (!n) {
            close(fd);
            wl_event_source_remove(server.reader);
            server.reader = NULL;
            wl_data_source_send_cancelled(server.source);
            if (server.reading_png) {
                assert(server.received == server.png_length);
                server.copied_png = true;
                assert(write(server.result_fd, "P", 1) == 1);
            } else {
                assert(server.received == 100001);
                server.copied_text = true;
                assert(write(server.result_fd, "C", 1) == 1);
                external_offer(false);
            }
            return 0;
        }
        for (ssize_t i = 0; i < n; ++i) {
            size_t p = server.received++;
            if (server.reading_png) {
                assert(p < server.png_length && block[i] == server.png[p]);
            } else
                assert(p < 100001 &&
                       block[i] == (p == 100000 ? '\n' : (unsigned char)('a' + p % 26)));
        }
    }
}
static void set_selection(struct wl_client *c, struct wl_resource *device,
                          struct wl_resource *source, uint32_t serial)
{
    (void)c;
    (void)device;
    if (!source) {
        if (serial == 99) {
            assert(server.copied_text && server.copied_png && server.finished_drop);
            wl_display_terminate(server.display);
        } else
            external_offer(true);
        return;
    }
    assert(!server.reader);
    int pipes[2];
    assert(!pipe2(pipes, O_CLOEXEC | O_NONBLOCK));
    server.source = source;
    server.read_fd = pipes[0];
    server.received = 0;
    struct test_source *s = wl_resource_get_user_data(source);
    server.reading_png = s->png;
    if (s->png)
        assert(s->fragment && !s->text); /* only the image, so other apps paste it */
    server.reader = wl_event_loop_add_fd(wl_display_get_event_loop(server.display), pipes[0],
                                         WL_EVENT_READABLE, source_read, NULL);
    assert(server.reader);
    wl_data_source_send_send(source, s->png ? "image/png" : "text/plain;charset=utf-8", pipes[1]);
    close(pipes[1]);
}
static void start_drag(struct wl_client *c, struct wl_resource *r, struct wl_resource *source,
                       struct wl_resource *origin, struct wl_resource *icon, uint32_t serial)
{
    (void)c;
    (void)r;
    (void)source;
    (void)origin;
    (void)icon;
    (void)serial;
    assert(0);
}
static const struct wl_data_device_interface device_impl = {
    .start_drag = start_drag, .set_selection = set_selection, .release = destroy_resource};
static void new_device(struct wl_client *c, struct wl_resource *manager, uint32_t id,
                       struct wl_resource *seat)
{
    (void)seat;
    server.device =
        wl_resource_create(c, &wl_data_device_interface, wl_resource_get_version(manager), id);
    assert(server.device);
    wl_resource_set_implementation(server.device, &device_impl, NULL, NULL);
}
static const struct wl_data_device_manager_interface manager_impl = {
    .create_data_source = new_source, .get_data_device = new_device};
static void bind_manager(struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource *r =
        wl_resource_create(c, &wl_data_device_manager_interface, (int)version, id);
    assert(r);
    wl_resource_set_implementation(r, &manager_impl, NULL, NULL);
}
static const struct wl_seat_interface seat_impl = {.release = destroy_resource};
static void bind_seat(struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &wl_seat_interface, (int)version, id);
    assert(r);
    wl_resource_set_implementation(r, &seat_impl, NULL, NULL);
    wl_seat_send_capabilities(r, 0);
}
static void pump_sends(struct wayland_client *c)
{
    for (unsigned attempts = 0; attempts < 1000; ++attempts) {
        bool pending = false;
        for (struct clipboard_source *s = c->sources; s; s = s->next)
            for (unsigned i = 0; i < 4; ++i)
                if (s->sends[i].fd >= 0) {
                    pending = true;
                    send_ready(s, i);
                }
        if (!pending)
            return;
        struct timespec t = {.tv_nsec = 1000000};
        nanosleep(&t, NULL);
    }
    assert(0);
}
int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    setenv("MIB_OCR", "0", 1);
    char directory[] = "/tmp/mib-clipboard-XXXXXX";
    assert(mkdtemp(directory));
    char path[1024];
    snprintf(path, sizeof(path), "%s/input.png", directory);
    const unsigned char rgba[] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 0, 255};
    size_t n;
    unsigned char *png = image_png(rgba, 2, 2, &n);
    assert(png);
    assert(session_write_new(path, (char *)png, n));
    int sockets[2], results[2];
    assert(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets));
    assert(!pipe2(results, O_CLOEXEC));
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        close(sockets[0]);
        close(results[0]);
        server = (struct test_server){.result_fd = results[1], .png = png, .png_length = n};
        snprintf(server.uri, sizeof(server.uri), "file://%s\r\n", path);
        server.display = wl_display_create();
        assert(server.display);
        assert(wl_global_create(server.display, &wl_seat_interface, 1, NULL, bind_seat));
        assert(wl_global_create(server.display, &wl_data_device_manager_interface, 3, NULL,
                                bind_manager));
        assert(
            wl_global_create(server.display, &wl_compositor_interface, 1, NULL, bind_compositor));
        assert(wl_client_create(server.display, sockets[1]));
        wl_display_run(server.display);
        wl_display_destroy_clients(server.display);
        wl_display_destroy(server.display);
        _exit(0);
    }
    close(sockets[1]);
    close(results[1]);
    free(png);
    struct app app;
    assert(app_open(&app));
    app.directory = strdup(directory);
    assert(app.directory);
    struct wayland_client c = {.app = &app, .receive_fd = -1, .serial = 1};
    c.display = wl_display_connect_to_fd(sockets[0]);
    assert(c.display);
    c.registry = wl_display_get_registry(c.display);
    wl_registry_add_listener(c.registry, &registry_listener, &c);
    assert(wl_display_roundtrip(c.display) >= 0);
    assert(c.seat && c.data_manager);
    c.data_device = wl_data_device_manager_get_data_device(c.data_manager, c.seat);
    wl_data_device_add_listener(c.data_device, &data_listener, &c);
    app.service = linux_service;
    app.service_data = &c;
    c.surface = wl_compositor_create_surface(c.compositor);
    char *text = malloc(100001);
    assert(text);
    for (size_t p = 0; p < 100000; ++p)
        text[p] = (char)('a' + p % 26);
    text[100000] = 0;
    assert(editor_replace(&app.editor, 0, 0, text, 100000));
    free(text);
    app.editor.cursor = 0;
    app_input(&app, KEY_TEXT, "y");
    app_input(&app, KEY_TEXT, "y");
    assert(wl_display_roundtrip(c.display) >= 0);
    pump_sends(&c);
    char result;
    assert(read(results[0], &result, 1) == 1 && result == 'C');
    assert(wl_display_roundtrip(c.display) >= 0);
    assert(editor_replace(&app.editor, 0, app.editor.length, "", 0));
    app_input(&app, KEY_TEXT, "p");
    assert(wl_display_roundtrip(c.display) >= 0);
    receive_ready(&c);
    assert(!strcmp(app.editor.text, "pasted from fake Wayland\n"));
    assert(editor_replace(&app.editor, 0, app.editor.length, "", 0));
    wl_data_device_set_selection(c.data_device, NULL, 2);
    assert(wl_display_roundtrip(c.display) >= 0);
    assert(wl_display_roundtrip(c.display) >= 0);
    receive_ready(&c);
    assert(media_at(&app, app.editor.cursor));
    assert(wl_display_roundtrip(c.display) >= 0);
    assert(read(results[0], &result, 1) == 1 && result == 'F');
    /* A single y copies the selected image, as does the context menu's Copy Image. */
    app_input(&app, KEY_TEXT, "y");
    assert(!app.editor.pending[0] && !strcmp(app.toast, "Image copied"));
    assert(wl_display_roundtrip(c.display) >= 0);
    pump_sends(&c);
    assert(read(results[0], &result, 1) == 1 && result == 'P');
    app.context_menu = true;
    app.context_choice = 0;
    app_input(&app, KEY_DOWN, "");
    app_input(&app, KEY_UP, "");
    app_input(&app, KEY_ENTER, "");
    assert(!app.context_menu);
    assert(wl_display_roundtrip(c.display) >= 0);
    pump_sends(&c);
    assert(read(results[0], &result, 1) == 1 && result == 'P');
    char *asset = media_target(&app);
    assert(asset);
    wl_data_device_set_selection(c.data_device, NULL, 99);
    wl_display_flush(c.display);
    int status;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    close(results[0]);
    receive_end(&c, false);
    offer_unref(c.pending_offer);
    offer_unref(c.selection_offer);
    offer_unref(c.drag_offer);
    while (c.sources)
        source_free(c.sources);
    wl_surface_destroy(c.surface);
    wl_compositor_destroy(c.compositor);
    wl_data_device_destroy(c.data_device);
    wl_data_device_manager_destroy(c.data_manager);
    wl_seat_destroy(c.seat);
    wl_registry_destroy(c.registry);
    wl_display_disconnect(c.display);
    app.service = NULL;
    app_close(&app);
    unlink(asset);
    free(asset);
    unlink(path);
    snprintf(path, sizeof(path), "%s/assets", directory);
    rmdir(path);
    rmdir(directory);
    puts("Private Wayland server: large text send, plain-text paste, URI drop, PNG copy and DND "
         "finish passed");
}
