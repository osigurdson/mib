#define _POSIX_C_SOURCE 200809L
#include "media.h"
#include "app.h"
#include "edit-internal.h"
#include "platform.h"
#include "session.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include "platform/windows/win32.h"
#else
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif
#define IMAGE_LIMIT 256
#define FILE_LIMIT (32 * 1024 * 1024)
struct asset {
    struct asset *next;
    char name[256];
};
struct media {
    struct asset *assets;
    unsigned asset_count;
    struct media_entry *entries;
    unsigned count;
    uint64_t revision;
    bool ocr_unavailable;
    char job_name[256];
#ifdef _WIN32
    HANDLE job;
#else
    pid_t job;
#endif
};
/* Keep a bounded set of assets seen in this session, including imports deleted
 * before their first paint. Overflow conservatively leaves extra files alone. */
static void track_asset(struct app *app, const char *name)
{
    if (!app->media)
        app->media = calloc(1, sizeof(*app->media));
    struct media *m = app->media;
    if (!m)
        return;
    for (struct asset *a = m->assets; a; a = a->next)
        if (!strcmp(a->name, name))
            return;
    if (m->asset_count >= IMAGE_LIMIT)
        return;
    struct asset *a = malloc(sizeof(*a));
    if (!a)
        return;
    strcpy(a->name, name);
    a->next = m->assets;
    m->assets = a;
    ++m->asset_count;
}
static void stop_job(struct media *m)
{
    if (!m->job)
        return;
#ifdef _WIN32
    TerminateProcess(m->job, 0);
    WaitForSingleObject(m->job, INFINITE);
    CloseHandle(m->job);
    m->job = NULL;
#else
    kill(m->job, SIGTERM);
    while (waitpid(m->job, NULL, 0) < 0 && errno == EINTR) {
    }
    m->job = 0;
#endif
}
void media_saved(void *context)
{
    struct app *app = context;
    struct media *m = app->media;
    if (!m)
        return;
    bool cleaned = false, failed = false;
    struct asset **link = &m->assets;
    while (*link) {
        struct asset *a = *link;
        /* Retain even literal/inline references and matches beyond IMAGE_LIMIT.
         * False positives are preferable to removing a file still in use. */
        if (strstr(app->editor.text, a->name)) {
            link = &a->next;
            continue;
        }
        if (m->job && !strcmp(m->job_name, a->name))
            stop_job(m); /* Do not let OCR recreate sidecars after cleanup. */
        bool removed = true;
        const char *suffixes[] = {"", ".txt", ".tsv"};
        for (unsigned i = 0; i < 3; ++i) {
            char relative[272];
            snprintf(relative, sizeof(relative), "%s%s", a->name, suffixes[i]);
            char *path = session_path(app, relative);
            bool ok = path && platform_file_remove(path);
            free(path);
            if (i == 0 && ok) {
                cleaned = true;
                editor_forget_registers(&app->editor, a->name);
            }
            removed &= ok;
            if (i == 0 && !ok)
                break;
        }
        if (!removed) {
            failed = true;
            link = &a->next;
            continue;
        }
        *link = a->next;
        free(a);
        --m->asset_count;
    }
    if (cleaned) {
        editor_history_clear(&app->editor);
        if (app->editor.mode == INSERT)
            editor_change_begin(&app->editor);
    }
    if (cleaned || failed)
        snprintf(app->editor.message, sizeof(app->editor.message), "Written; %s%s",
                 cleaned ? "image cleanup reset undo/redo" : "",
                 failed ? " (some assets could not be removed)" : "");
}
/* Obsidian's size suffix: "alt|300", "alt|300x200" or a bare "300". Returns the width and
 * where the suffix starts, or 0 when there is none. The height is ignored: Jot keeps the
 * aspect ratio. */
static int alt_width(const char *alt, size_t n, size_t *suffix)
{
    size_t from = n;
    while (from && alt[from - 1] != '|')
        --from;
    size_t i = from;
    long width = 0;
    while (i < n && isdigit((unsigned char)alt[i]) && width <= 100000)
        width = width * 10 + (alt[i++] - '0');
    if (i == from || width < 1 || width > 100000)
        return 0;
    if (i < n && alt[i] == 'x') {
        size_t digits = ++i;
        while (i < n && isdigit((unsigned char)alt[i]))
            ++i;
        if (i == digits)
            return 0;
    }
    if (i != n)
        return 0;
    *suffix = from ? from - 1 : 0;
    return (int)width;
}
static bool parse(const struct editor *ed, size_t start, size_t end, char *name)
{
    if (end - start < 14 || ed->text[start] != '!' || ed->text[start + 1] != '[' ||
        ed->text[end - 1] != ')')
        return false;
    const char *p = memchr(ed->text + start + 2, ']', end - start - 2);
    if (!p || p[1] != '(' || strncmp(p + 2, "assets/", 7))
        return false;
    p += 9;
    size_t length = (size_t)((ed->text + end - 1) - p);
    if (!length || length > 248 || (length == 1 && p[0] == '.') ||
        (length == 2 && !memcmp(p, "..", 2)))
        return false;
    for (size_t i = 0; i < length; ++i)
        if (p[i] == '/' || p[i] == '\\' || p[i] == '\n' || p[i] == '\r' || p[i] == ')')
            return false;
    memcpy(name, "assets/", 7);
    memcpy(name + 7, p, length);
    name[length + 7] = 0;
    return true;
}
void media_sync(struct app *app)
{
    struct media *old = app->media;
    struct editor *ed = &app->editor;
    if (app->media_scanned && app->media_revision == ed->revision)
        return;
    app->media_scanned = true;
    app->media_revision = ed->revision;
    if (!app->directory)
        return;
    struct media_entry *entries = NULL;
    unsigned count = 0, capacity = 0;
    for (size_t p = 0; p < ed->length;) {
        size_t end = editor_line_end(ed, p);
        char name[256];
        if (count < IMAGE_LIMIT && parse(ed, p, end, name)) {
            if (count == capacity) {
                unsigned next = capacity ? capacity * 2 : 8;
                struct media_entry *q = realloc(entries, next * sizeof(*q));
                if (!q)
                    break;
                entries = q;
                capacity = next;
            }
            struct media_entry e = {.start = p, .end = end};
            strcpy(e.name, name);
            const char *close = memchr(ed->text + p + 2, ']', end - p - 2);
            e.alt_end = (size_t)(close - ed->text);
            size_t suffix = 0;
            e.size = alt_width(ed->text + p + 2, e.alt_end - p - 2, &suffix);
            e.alt_size = e.size ? p + 2 + suffix : e.alt_end;
            bool cached = false;
            if (old)
                for (unsigned i = 0; i < old->count; ++i)
                    if (!strcmp(old->entries[i].name, name)) {
                        e.width = old->entries[i].width;
                        e.height = old->entries[i].height;
                        e.ocr_done = old->entries[i].ocr_done;
                        cached = true;
                        break;
                    }
            if (!cached) {
                char *path = session_path(app, name);
                bool missing;
                char error[128];
                FILE *file =
                    path ? platform_file_open_read(path, &missing, error, sizeof(error)) : NULL;
                free(path);
                if (file) {
                    unsigned char header[65536];
                    size_t n = fread(header, 1, sizeof(header), file);
                    fclose(file);
                    if (!image_info(header, n, &e.width, &e.height))
                        e.width = e.height = 0;
                }
            }
            entries[count++] = e;
        }
        if (end == ed->length)
            break;
        p = end + 1;
    }
    if (!old && !count) {
        free(entries);
        return;
    }
    if (!old) {
        old = calloc(1, sizeof(*old));
        if (!old) {
            free(entries);
            return;
        }
        app->media = old;
    }
    for (unsigned i = 0; i < count; ++i)
        track_asset(app, entries[i].name);
    free(old->entries);
    old->entries = entries;
    old->count = count;
    old->revision = ed->revision;
}
const struct media_entry *media_at(struct app *app, size_t p)
{
    media_sync(app);
    if (!app->media)
        return NULL;
    for (unsigned i = 0; i < app->media->count; ++i) {
        const struct media_entry *e = &app->media->entries[i];
        if (p >= e->start && p <= e->end)
            return e;
    }
    return NULL;
}
void media_display_size(struct app *app, const struct media_entry *e, int available, int *width,
                        int *height)
{
    *width = *height = 0;
    if (e->width < 1 || e->height < 1)
        return;
    int64_t w = e->width;
    uint32_t scale = app->scale120 ? app->scale120 : 120;
    if (app->image_drag && app->image_box_start == e->start)
        w = app->drag_width;
    else if (e->size)
        w = (int64_t)e->size * scale / 120;
    if (available > 0 && w > available)
        w = available;
    if (w < 1)
        w = 1;
    int64_t h = (int64_t)e->height * w / e->width;
    *width = (int)w;
    *height = h > 0 ? (int)h : 1;
}
bool media_resize(struct editor *ed, size_t position, const char *spec, void *data)
{
    struct app *app = data;
    const struct media_entry *found = media_at(app, position);
    if (!found)
        return false;
    struct media_entry e = *found;
    if (!editor_can_edit(ed))
        return true;
    if (e.width < 1 || e.height < 1) {
        snprintf(ed->message, sizeof(ed->message), "Cannot read the image's size");
        return true;
    }
    /* Widths are logical pixels, as in Obsidian; an unsized image shows at one image pixel
     * per physical pixel. */
    int64_t scale = app->scale120 ? app->scale120 : 120;
    int shown, ignored;
    media_display_size(app, &e, app->text_width, &shown, &ignored);
    int64_t current = (shown * 120 + scale / 2) / scale;
    int64_t natural = (e.width * 120 + scale / 2) / scale;
    int64_t widest = app->text_width > 0 ? app->text_width * 120 / scale : 100000;
    const char *s = spec;
    while (*s == ' ')
        ++s;
    int64_t width = 0;
    if (*s) {
        char sign = *s == '+' || *s == '-' ? *s++ : 0;
        int64_t n = 0;
        const char *digits = s;
        while (isdigit((unsigned char)*s) && n <= 100000)
            n = n * 10 + (*s++ - '0');
        bool percent = *s == '%';
        if (percent)
            ++s;
        while (*s == ' ')
            ++s;
        if (s == digits || *s || n > 100000) {
            snprintf(ed->message, sizeof(ed->message), "Use :resize, :resize N, N%%, +N or -N");
            return true;
        }
        int64_t amount = percent ? ((sign ? current : natural) * n + 50) / 100 : n;
        width = sign == '+' ? current + amount : sign == '-' ? current - amount : amount;
        if (sign && amount && width == current)
            width += sign == '+' ? 1 : -1;
        if (width || sign) {
            if (width > widest)
                width = widest;
            if (width < 16)
                width = 16;
        }
    }
    char suffix[16] = "";
    if (width)
        snprintf(suffix, sizeof(suffix), "|%d", (int)width);
    size_t cursor = ed->cursor;
    if (!editor_replace(ed, e.alt_size, e.alt_end, suffix, strlen(suffix)))
        return true;
    ed->cursor = cursor < e.alt_size ? cursor : e.start;
    if (width)
        snprintf(ed->message, sizeof(ed->message), "Image width %d", (int)width);
    else
        snprintf(ed->message, sizeof(ed->message), "Image at its original size");
    return true;
}
int media_rows(struct app *app, size_t p, int width, int line_height, int max_rows)
{
    const struct media_entry *e = media_at(app, p);
    if (!e || e->width < 1 || e->height < 1 || width < 1 || line_height < 1)
        return 1;
    int w, h;
    media_display_size(app, e, width, &w, &h);
    /* The renderer keeps one row and 4 pixels of padding around the image. */
    int rows = (h + 4 + line_height - 1) / line_height + 1;
    int maximum = max_rows > 1 ? max_rows - 1 : 1;
    if (rows > maximum)
        rows = maximum;
    return rows > 1 ? rows : 1;
}
bool media_pixels(struct app *app, const struct media_entry *e, struct image *image)
{
    char *path = session_path(app, e->name);
    size_t n;
    char *bytes = path ? session_read(path, FILE_LIMIT, &n) : NULL;
    free(path);
    bool ok = bytes && image_decode(image, (unsigned char *)bytes, n);
    free(bytes);
    return ok;
}
static bool contains(const char *text, size_t length, const char *pattern)
{
    size_t n = strlen(pattern);
    if (!n || n > length)
        return false;
    bool sensitive = false;
    for (size_t i = 0; i < n; ++i)
        if (isupper((unsigned char)pattern[i]))
            sensitive = true;
    for (size_t p = 0; p + n <= length; ++p) {
        size_t i = 0;
        for (; i < n; ++i) {
            unsigned char a = (unsigned char)text[p + i], b = (unsigned char)pattern[i];
            if (sensitive ? a != b : tolower(a) != tolower(b))
                break;
        }
        if (i == n)
            return true;
    }
    return false;
}
static char *sidecar(struct app *app, const struct media_entry *e, const char *suffix,
                     size_t *length)
{
    char relative[272];
    snprintf(relative, sizeof(relative), "%s%s", e->name, suffix);
    char *path = session_path(app, relative);
    char *text = path ? session_read(path, 2 * 1024 * 1024, length) : NULL;
    free(path);
    return text;
}
size_t media_search(const struct editor *ed, size_t origin, bool forward, void *context)
{
    struct app *app = context;
    media_sync(app);
    app->editor.search_pending = app->work_at != 0;
    if (!app->media || !ed->search[0])
        return SIZE_MAX;
    size_t best = SIZE_MAX, distance = SIZE_MAX;
    for (unsigned i = 0; i < app->media->count; ++i) {
        const struct media_entry *e = &app->media->entries[i];
        size_t n;
        char *text = sidecar(app, e, ".txt", &n);
        bool found = text && contains(text, n, ed->search);
        free(text);
        if (!found)
            continue;
        size_t d =
            forward ? (e->start > origin ? e->start - origin : ed->length - origin + e->start + 1)
                    : (e->start < origin ? origin - e->start : origin + ed->length - e->start + 1);
        if (d < distance) {
            distance = d;
            best = e->start;
        }
    }
    return best;
}
char *media_ocr_text(struct app *app, size_t *length)
{
    const struct media_entry *e = media_at(app, app->editor.cursor);
    return e ? sidecar(app, e, ".txt", length) : NULL;
}
char *media_clipboard_image(struct app *app, size_t *length)
{
    if (!app->clipboard_image)
        return NULL;
    char *bytes = session_read(app->clipboard_image, FILE_LIMIT, length);
    if (!bytes)
        return NULL;
    if (*length >= 8 && !memcmp(bytes, "\x89PNG\r\n\x1a\n", 8))
        return bytes;
    struct image image = {0};
    if (!image_decode(&image, (unsigned char *)bytes, *length)) {
        free(bytes);
        return NULL;
    }
    free(bytes);
    unsigned char *png = image_png(image.rgba, image.width, image.height, length);
    image_free(&image);
    return (char *)png;
}
char *media_target(struct app *app)
{
    const struct media_entry *e = media_at(app, app->editor.cursor);
    if (e)
        return session_path(app, e->name);
    struct editor *ed = &app->editor;
    size_t start = editor_line_start(ed, ed->cursor), end = editor_line_end(ed, ed->cursor);
    for (size_t p = start; p < end; ++p) {
        size_t a = p, b = p;
        if (p + 1 < end && ed->text[p] == ']' && ed->text[p + 1] == '(') {
            a = p + 2;
            b = a;
            while (b < end && ed->text[b] != ')')
                ++b;
            size_t left = p;
            while (left > start && ed->text[left] != '[')
                --left;
            if (ed->cursor < left || ed->cursor > b)
                continue;
        } else if (!strncmp(ed->text + p, "https://", 8) || !strncmp(ed->text + p, "http://", 7)) {
            a = p;
            while (b < end && !isspace((unsigned char)ed->text[b]) && ed->text[b] != ')' &&
                   ed->text[b] != '>')
                ++b;
            if (ed->cursor < a || ed->cursor > b)
                continue;
        } else
            continue;
        if (b - a > 4096)
            return NULL;
        char *target = malloc(b - a + 1);
        if (!target)
            return NULL;
        memcpy(target, ed->text + a, b - a);
        target[b - a] = 0;
        if (strncmp(target, "https://", 8) && strncmp(target, "http://", 7) &&
            strncmp(target, "mailto:", 7)) {
            char *path = session_path(app, target);
            free(target);
            target = path;
        }
        return target;
    }
    return NULL;
}
static bool ensure_directory(struct app *app)
{
    app->media_scanned = false;
    if (!app->directory) {
        if (!app->editor.filename) {
            snprintf(app->editor.message, sizeof(app->editor.message),
                     "Save to a file or open a page before pasting images");
            return false;
        }
        const char *file = app->editor.filename, *slash = strrchr(file, '/'),
                   *back = strrchr(file, '\\');
        if (back && (!slash || back > slash))
            slash = back;
        size_t n = slash ? (size_t)(slash - file) : 1;
        app->directory = malloc(n + 1);
        if (!app->directory)
            return false;
        if (slash)
            memcpy(app->directory, file, n);
        else
            app->directory[0] = '.';
        app->directory[n] = 0;
    }
    char *assets = session_path(app, "assets");
    bool ok = assets && session_mkdir(assets);
    free(assets);
    return ok;
}
static bool store_image(struct app *app, const unsigned char *data, size_t length, char name[256])
{
    int width, height;
    if (!image_info(data, length, &width, &height)) {
        snprintf(app->editor.message, sizeof(app->editor.message),
                 "Image is invalid or exceeds the 8 megapixel limit");
        return false;
    }
    if (!ensure_directory(app))
        return false;
    unsigned char *encoded = NULL;
    size_t bytes = length;
    if (length < 8 || memcmp(data, "\x89PNG\r\n\x1a\n", 8)) {
        struct image image = {0};
        if (!image_decode(&image, data, length))
            return false;
        encoded = image_png(image.rgba, image.width, image.height, &bytes);
        image_free(&image);
        if (!encoded)
            return false;
        data = encoded;
    }
    time_t now = time(NULL);
    struct tm tm;
#ifdef _WIN32
    struct tm *tmp = localtime(&now);
    if (tmp)
        tm = *tmp;
    else
        memset(&tm, 0, sizeof(tm));
#else
    localtime_r(&now, &tm);
#endif
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y%m%d-%H%M%S", &tm);
    static unsigned sequence;
    bool written = false;
    for (unsigned attempt = 0; attempt < 16 && !written; ++attempt) {
        snprintf(name, 256, "assets/%s-%08x.png", timestamp,
                 (unsigned)(session_now() ^ (++sequence * 2654435761u)));
        char *path = session_path(app, name);
        if (!path)
            break;
        bool missing;
        char error[128];
        FILE *file = platform_file_open_read(path, &missing, error, sizeof(error));
        if (file)
            fclose(file);
        if (!file && missing)
            written = session_write_exclusive(path, (const char *)data, bytes);
        if (written) { /* remembered for media_close */
            char **imports = realloc(app->imports, (app->import_count + 1) * sizeof(*imports));
            if (imports) {
                app->imports = imports;
                imports[app->import_count++] = path;
                path = NULL;
            }
        }
        free(path);
    }
    free(encoded);
    if (!written)
        return false;
    track_asset(app, name);
    return true;
}
bool media_paste_image(struct app *app, const unsigned char *data, size_t length)
{
    if (!editor_can_edit(&app->editor)) {
        app->dirty = true;
        return false;
    }
    char name[256];
    if (!store_image(app, data, length, name))
        return false;
    char markup[280];
    snprintf(markup, sizeof(markup), "![](%s)\n", name);
    struct editor *ed = &app->editor;
    size_t p = ed->cursor, a, b;
    bool lines;
    bool visual = editor_selection(ed, &a, &b, &lines), own = !editor_change_active(ed);
    if (!visual && ed->mode == INSERT && ed->view == VIEW_NORMAL) {
        const struct media_entry *image = media_at(app, p);
        if (image)
            p = image->start;
    }
    editor_change_begin(ed);
    if (visual) {
        editor_replace(ed, a, b, "", 0);
        p = a;
        ed->mode = INSERT;
    }
    if (ed->mode != INSERT) {
        p = editor_line_end(ed, p);
        if (p < ed->length)
            ++p;
    }
    if (p && ed->text[p - 1] != '\n') {
        editor_replace(ed, p, p, "\n", 1);
        ++p;
    }
    size_t at = p;
    editor_replace(ed, p, p, markup, strlen(markup));
    ed->cursor =
        !visual && ed->mode == INSERT && ed->view == VIEW_NORMAL ? at + strlen(markup) : at;
    if (visual)
        ed->mode = NORMAL;
    if (own)
        editor_change_end(ed);
    app->dirty = true;
    app->follow_cursor = true;
    media_sync(app);
    media_tick(app);
    return true;
}
bool media_prepare_insert(struct app *app)
{
    struct editor *ed = &app->editor;
    if (ed->view != VIEW_NORMAL || ed->mode != INSERT)
        return true;
    const struct media_entry *image = media_at(app, ed->cursor);
    if (!image)
        return true;
    size_t start = image->start;
    editor_change_begin(ed);
    if (!editor_replace(ed, start, start, "\n", 1))
        return false;
    ed->cursor = start;
    return true;
}
void media_paste_text(struct app *app, const char *text, size_t length)
{
    if (!editor_can_edit(&app->editor)) {
        app->dirty = true;
        return;
    }
    if (!length || length > 16 * 1024 * 1024 || memchr(text, 0, length))
        return;
    char *normal = malloc(length + 2);
    if (!normal)
        return;
    size_t n = 0;
    for (size_t i = 0; i < length; ++i) {
        if (text[i] == '\r' && i + 1 < length && text[i + 1] == '\n')
            continue;
        normal[n++] = text[i] == '\r' ? '\n' : text[i];
    }
    bool image_line = app->editor.view == VIEW_NORMAL && app->editor.mode == NORMAL &&
                      media_at(app, app->editor.cursor);
    if (image_line && normal[n - 1] != '\n')
        normal[n++] = '\n';
    normal[n] = 0;
    if (!media_prepare_insert(app)) {
        free(normal);
        return;
    }
    bool linewise = n && normal[n - 1] == '\n';
    editor_put(&app->editor, normal, n, linewise, app->editor.put_after,
               app->editor.put_count ? app->editor.put_count : 1);
    free(normal);
    app->dirty = true;
    app->follow_cursor = true;
}

static uint32_t fragment_u32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void fragment_write_u32(unsigned char *p, uint32_t n)
{
    for (unsigned i = 0; i < 4; ++i)
        p[i] = (unsigned char)(n >> (i * 8));
}
char *media_fragment(struct app *app, size_t *length)
{
    struct editor *ed = &app->editor;
    if (!app->directory || !ed->register_text || ed->register_length > 16 * 1024 * 1024)
        return NULL;
    struct editor snapshot = {.text = ed->register_text, .length = ed->register_length};
    bool has_image = false;
    for (size_t p = 0; p < snapshot.length;) {
        size_t end = editor_line_end(&snapshot, p);
        char name[256];
        if (parse(&snapshot, p, end, name)) {
            has_image = true;
            break;
        }
        p = end < snapshot.length ? end + 1 : end;
    }
    if (!has_image)
        return NULL;
    char *directory = session_absolute(app->directory);
    if (!directory)
        return NULL;
    size_t n = strlen(directory);
    if (n > 65535) {
        free(directory);
        return NULL;
    }
    size_t total = 16 + n + ed->register_length;
    unsigned char *bytes = malloc(total);
    if (bytes) {
        memcpy(bytes, "TVFRAG1", 8);
        fragment_write_u32(bytes + 8, (uint32_t)n);
        fragment_write_u32(bytes + 12, (uint32_t)ed->register_length);
        memcpy(bytes + 16, directory, n);
        memcpy(bytes + 16 + n, ed->register_text, ed->register_length);
        *length = total;
    }
    free(directory);
    return (char *)bytes;
}
bool media_paste_fragment(struct app *app, const unsigned char *bytes, size_t length)
{
    if (!editor_can_edit(&app->editor)) {
        app->dirty = true;
        return false;
    }
    if (length < 16 || memcmp(bytes, "TVFRAG1", 8))
        return false;
    size_t directory_length = fragment_u32(bytes + 8), text_length = fragment_u32(bytes + 12);
    if (!directory_length || directory_length > 65535 || text_length > 16 * 1024 * 1024 ||
        directory_length > length - 16 || text_length > length - 16 - directory_length)
        return false;
    if (memchr(bytes + 16, 0, directory_length + text_length))
        return false;
    char *directory = malloc(directory_length + 1), *text = malloc(text_length + 1);
    if (!directory || !text) {
        free(directory);
        free(text);
        return false;
    }
    memcpy(directory, bytes + 16, directory_length);
    directory[directory_length] = 0;
    memcpy(text, bytes + 16 + directory_length, text_length);
    text[text_length] = 0;
    struct editor input = {.text = text, .length = text_length}, output = {0};
    bool ok = true;
    unsigned images = 0;
    for (size_t p = 0; p < input.length;) {
        size_t end = editor_line_end(&input, p), next = end < input.length ? end + 1 : end;
        char name[256], new_name[256];
        if (images < IMAGE_LIMIT && parse(&input, p, end, name)) {
            size_t n = strlen(directory) + strlen(name) + 2;
            char *path = malloc(n);
            size_t file_length = 0;
            char *data = NULL;
            if (path) {
                snprintf(path, n, "%s/%s", directory, name);
                data = session_read(path, FILE_LIMIT, &file_length);
                free(path);
            }
            if (data && store_image(app, (unsigned char *)data, file_length, new_name)) {
                const char *close = memchr(input.text + p, ']', end - p);
                size_t prefix = (size_t)(close + 2 - (input.text + p));
                ok = editor_raw_replace(&output, output.length, output.length, input.text + p,
                                        prefix) &&
                     editor_raw_replace(&output, output.length, output.length, new_name,
                                        strlen(new_name)) &&
                     editor_raw_replace(&output, output.length, output.length, ")", 1);
                if (ok && next > end)
                    ok = editor_raw_replace(&output, output.length, output.length, "\n", 1);
                free(data);
                ++images;
                if (output.length > 16 * 1024 * 1024)
                    ok = false;
                if (!ok)
                    break;
                p = next;
                continue;
            }
            free(data);
        }
        if (!editor_raw_replace(&output, output.length, output.length, input.text + p, next - p)) {
            ok = false;
            break;
        }
        if (output.length > 16 * 1024 * 1024) {
            ok = false;
            break;
        }
        p = next;
    }
    if (ok)
        media_paste_text(app, output.text ? output.text : "", output.length);
    free(output.text);
    free(text);
    free(directory);
    if (ok)
        media_tick(app);
    return ok;
}
void media_import_file(struct app *app, const char *path)
{
    if (!editor_can_edit(&app->editor)) {
        app->dirty = true;
        return;
    }
    size_t n;
    char *data = session_read(path, FILE_LIMIT, &n);
    if (!data)
        return;
    int w, h;
    if (image_info((unsigned char *)data, n, &w, &h))
        media_paste_image(app, (unsigned char *)data, n);
    else if (!memchr(data, 0, n))
        media_paste_text(app, data, n);
    free(data);
}
void media_import_uris(struct app *app, const char *uris, size_t length)
{
    if (!editor_can_edit(&app->editor)) {
        app->dirty = true;
        return;
    }
    size_t p = 0;
    while (p < length) {
        size_t end = p;
        while (end < length && uris[end] != '\r' && uris[end] != '\n')
            ++end;
        if (end - p > 7 && !memcmp(uris + p, "file://", 7)) {
            size_t a = p + 7;
            if (end - a >= 10 && !memcmp(uris + a, "localhost/", 10))
                a += 9;
            if (a < end && uris[a] == '/') {
#ifdef _WIN32
                if (a + 2 < end && uris[a + 2] == ':')
                    ++a;
#endif
                char *path = malloc(end - a + 1);
                if (path) {
                    size_t n = 0;
                    for (size_t i = a; i < end; ++i) {
                        unsigned char c = (unsigned char)uris[i];
                        if (c == '%' && i + 2 < end && isxdigit((unsigned char)uris[i + 1]) &&
                            isxdigit((unsigned char)uris[i + 2])) {
                            char hex[3] = {uris[i + 1], uris[i + 2], 0};
                            c = (unsigned char)strtoul(hex, NULL, 16);
                            i += 2;
                        }
                        if (!c) {
                            n = 0;
                            break;
                        }
                        path[n++] = (char)c;
                    }
                    path[n] = 0;
                    if (n)
                        media_import_file(app, path);
                    free(path);
                }
            }
        }
        p = end;
        while (p < length && (uris[p] == '\r' || uris[p] == '\n'))
            ++p;
    }
}
static bool spawn_ocr(struct app *app, const struct media_entry *e)
{
    char *path = session_path(app, e->name);
    if (!path)
        return false;
#ifdef _WIN32
    wchar_t executable[32768];
    DWORD found = SearchPathW(NULL, L"tesseract.exe", NULL, 32768, executable, NULL);
    wchar_t *w = windows_path(path);
    free(path);
    if (!found || found >= 32768 || !w) {
        free(w);
        app->media->ocr_unavailable = true;
        return false;
    }
    size_t n = wcslen(w) * 2 + 64;
    wchar_t *command = malloc(n * sizeof(wchar_t));
    if (!command) {
        free(w);
        return false;
    }
    swprintf(command, n, L"tesseract.exe \"%ls\" \"%ls\" txt tsv", w, w);
    free(w);
    STARTUPINFOW startup = {.cb = sizeof(startup)};
    PROCESS_INFORMATION process;
    bool ok = CreateProcessW(executable, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL,
                             &startup, &process) != 0;
    free(command);
    if (ok) {
        CloseHandle(process.hThread);
        app->media->job = process.hProcess;
    }
#else
    char *arguments[] = {"tesseract", path, path, "txt", "tsv", NULL};
    pid_t pid;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", 0, 0);
    posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", 1, 0);
    posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", 1, 0);
    mode_t previous_umask = umask(0077);
    int error = posix_spawnp(&pid, "tesseract", &actions, NULL, arguments, environ);
    umask(previous_umask);
    posix_spawn_file_actions_destroy(&actions);
    free(path);
    bool ok = error == 0;
    if (ok)
        app->media->job = pid;
    else
        app->media->ocr_unavailable = true;
#endif
    if (ok)
        strcpy(app->media->job_name, e->name);
    return ok;
}
void media_tick(struct app *app)
{
    media_sync(app);
    struct media *m = app->media;
    app->work_at = 0;
    if (!m)
        return;
    if (m->job) {
#ifdef _WIN32
        bool done = WaitForSingleObject(m->job, 0) != WAIT_TIMEOUT;
        if (done) {
            CloseHandle(m->job);
            m->job = NULL;
        }
#else
        int status;
        pid_t result = waitpid(m->job, &status, WNOHANG);
        bool done = result > 0 || (result < 0 && errno != EINTR);
        if (done)
            m->job = 0;
#endif
        if (!done) {
            app->work_at = session_now() + 100;
            return;
        }
        for (unsigned i = 0; i < m->count; ++i)
            if (!strcmp(m->entries[i].name, m->job_name))
                m->entries[i].ocr_done = true;
        app->dirty = true;
        if (app->ocr_retry && app->editor.search[0]) {
            app->ocr_retry = false;
            bool prompt = app->editor.command_active && app->editor.command_kind != ':';
            if (prompt)
                app->editor.cursor = app->editor.search_origin;
            editor_search(&app->editor,
                          prompt ? app->editor.command_kind == '/' : app->editor.search_forward, 1);
            const struct media_entry *hit = media_at(app, app->editor.cursor);
            app->image_hit =
                !app->editor.search_failed && hit && app->editor.match_start == hit->start;
        }
    }
    const char *enabled = getenv("TINYVI_OCR");
    if (m->ocr_unavailable || (enabled && !strcmp(enabled, "0")))
        return;
    for (unsigned i = 0; i < m->count; ++i) {
        struct media_entry *e = &m->entries[i];
        if (e->ocr_done || e->width < 1)
            continue;
        size_t n;
        char *text = sidecar(app, e, ".txt", &n);
        if (text) {
            free(text);
            char *tsv = sidecar(app, e, ".tsv", &n);
            if (tsv) {
                free(tsv);
                e->ocr_done = true;
                continue;
            }
        }
        e->ocr_done = true;
        if (spawn_ocr(app, e)) {
            app->work_at = session_now() + 100;
            break;
        }
        if (m->ocr_unavailable)
            break;
    }
}
void media_free(struct media *m)
{
    if (!m)
        return;
    stop_job(m);
    while (m->assets) {
        struct asset *next = m->assets->next;
        free(m->assets);
        m->assets = next;
    }
    free(m->entries);
    free(m);
}
/* Whether a saved note in this window's notes folder (or its explicit file) mentions the
 * image, by its unique file name. Unreadable notes count as mentioning it. */
static bool saved_reference(const struct app *app, const char *image)
{
    const char *slash = strrchr(image, '/'), *backslash = strrchr(image, '\\');
    const char *name = slash > backslash ? slash + 1 : backslash ? backslash + 1 : image;
    for (unsigned page = 0; page < (app->root ? PAGE_COUNT : 1); ++page) {
        char *path = NULL;
        if (app->root) {
            size_t n = strlen(app->root) + 16;
            if ((path = malloc(n)))
                snprintf(path, n, "%s/%u/jot.md", app->root, page + 1);
        } else if (app->editor.filename) {
            size_t n = strlen(app->editor.filename) + 1;
            if ((path = malloc(n)))
                memcpy(path, app->editor.filename, n);
        }
        struct editor note = {0};
        bool found = !path || !editor_open(&note, path) || strstr(note.text, name);
        editor_destroy(&note);
        free(path);
        if (found)
            return true;
    }
    return false;
}
void media_close(struct app *app)
{
    /* Stop OCR on every page first, so it can't recreate sidecars removed below. */
    media_free(app->media);
    app->media = NULL;
    for (unsigned i = 0; i < PAGE_COUNT; ++i) {
        media_free(app->pages[i].media);
        app->pages[i].media = NULL;
    }
    /* A pasted image is written at once. If the window closes without a saved note using it
     * (the paste was undone, deleted or discarded), remove it and its OCR text. */
    for (unsigned i = 0; i < app->import_count; ++i) {
        if (!saved_reference(app, app->imports[i])) {
            const char *suffixes[] = {"", ".txt", ".tsv"};
            for (unsigned s = 0; s < 3; ++s) {
                char path[4096];
                int n = snprintf(path, sizeof(path), "%s%s", app->imports[i], suffixes[s]);
                if (n > 0 && (size_t)n < sizeof(path))
                    platform_file_remove(path);
            }
        }
        free(app->imports[i]);
    }
    free(app->imports);
    app->imports = NULL;
    app->import_count = 0;
}

struct ocr_word {
    size_t first, last;
    int x, y, w, h;
};
static void highlight_rectangle(const struct media_entry *e, uint32_t *pixels, int width,
                                int height, int stride, int x, int y, int draw_width,
                                int draw_height, int left, int top, int right, int bottom)
{
    int x0 = x + (int)((int64_t)left * draw_width / e->width),
        y0 = y + (int)((int64_t)top * draw_height / e->height);
    int x1 = x + (int)((int64_t)right * draw_width / e->width),
        y1 = y + (int)((int64_t)bottom * draw_height / e->height);
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > width)
        x1 = width;
    if (y1 > height)
        y1 = height;
    for (int py = y0; py < y1; ++py)
        for (int px = x0; px < x1; ++px) {
            uint32_t c = pixels[(size_t)py * stride + px];
            pixels[(size_t)py * stride + px] = 0xff000000 | ((((c >> 16) & 255) + 255) / 2 << 16) |
                                               ((((c >> 8) & 255) + 210) / 2 << 8) |
                                               ((c & 255) / 2);
        }
}
static void highlight_line(struct app *app, const struct media_entry *e, const char *text,
                           size_t length, const struct ocr_word *words, unsigned count,
                           uint32_t *pixels, int width, int height, int stride, int x, int y,
                           int dw, int dh)
{
    const char *pattern = app->editor.search;
    size_t n = strlen(pattern);
    bool sensitive = false;
    for (size_t i = 0; i < n; ++i)
        if (isupper((unsigned char)pattern[i]))
            sensitive = true;
    for (size_t p = 0; n && p + n <= length; ++p) {
        size_t i = 0;
        for (; i < n; ++i) {
            unsigned char a = (unsigned char)text[p + i], b = (unsigned char)pattern[i];
            if (sensitive ? a != b : tolower(a) != tolower(b))
                break;
        }
        if (i != n)
            continue;
        if (app->editor.search_word &&
            ((p && (isalnum((unsigned char)text[p - 1]) || text[p - 1] == '_')) ||
             (p + n < length && (isalnum((unsigned char)text[p + n]) || text[p + n] == '_'))))
            continue;
        int left = e->width, top = e->height, right = 0, bottom = 0;
        for (unsigned w = 0; w < count; ++w)
            if (words[w].last > p && words[w].first < p + n) {
                if (words[w].x < left)
                    left = words[w].x;
                if (words[w].y < top)
                    top = words[w].y;
                if (words[w].x + words[w].w > right)
                    right = words[w].x + words[w].w;
                if (words[w].y + words[w].h > bottom)
                    bottom = words[w].y + words[w].h;
            }
        if (right > left && bottom > top)
            highlight_rectangle(e, pixels, width, height, stride, x, y, dw, dh, left, top, right,
                                bottom);
    }
}
void media_highlight(struct app *app, const struct media_entry *e, uint32_t *pixels, int width,
                     int height, int stride, int x, int y, int draw_width, int draw_height)
{
    if (app->editor.search_hidden || app->editor.mode == INSERT || !app->image_hit ||
        !app->editor.search[0] || app->editor.cursor != e->start || e->width < 1 || e->height < 1)
        return;
    size_t bytes;
    char *tsv = sidecar(app, e, ".tsv", &bytes);
    if (!tsv)
        return;
    struct ocr_word words[256];
    unsigned count = 0;
    char text[8192];
    size_t length = 0;
    int key[4] = {0};
    for (char *p = tsv; *p;) {
        char *end = strchr(p, '\n');
        if (!end)
            end = p + strlen(p);
        char saved = *end;
        *end = 0;
        int level, page, block, paragraph, line, word, left, top, w, h, text_at = 0;
        float confidence;
        if (sscanf(p, "%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%f\t%n", &level, &page, &block,
                   &paragraph, &line, &word, &left, &top, &w, &h, &confidence, &text_at) == 11 &&
            text_at > 0 && level == 5 && left >= 0 && top >= 0 && w > 0 && h > 0 &&
            left <= e->width && top <= e->height && w <= e->width - left && h <= e->height - top) {
            size_t n = strlen(p + text_at);
            bool changed =
                key[0] != page || key[1] != block || key[2] != paragraph || key[3] != line;
            if (count && (changed || count == 256 || length + n + 1 >= sizeof(text))) {
                highlight_line(app, e, text, length, words, count, pixels, width, height, stride, x,
                               y, draw_width, draw_height);
                count = 0;
                length = 0;
            }
            key[0] = page;
            key[1] = block;
            key[2] = paragraph;
            key[3] = line;
            if (n && n + 1 < sizeof(text)) {
                if (length)
                    text[length++] = ' ';
                size_t first = length;
                memcpy(text + length, p + text_at, n);
                length += n;
                words[count++] = (struct ocr_word){first, length, left, top, w, h};
            }
        }
        *end = saved;
        if (!saved)
            break;
        p = end + 1;
    }
    if (count)
        highlight_line(app, e, text, length, words, count, pixels, width, height, stride, x, y,
                       draw_width, draw_height);
    free(tsv);
}
