#define _POSIX_C_SOURCE 200809L
#include "media.h"
#include "app.h"
#include "platform.h"
#include "session.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include "platform/windows/win32.h"
#else
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif
#define IMAGE_LIMIT 256
#define FILE_LIMIT (32 * 1024 * 1024)
struct media {
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
int media_rows(struct app *app, size_t p, int width, int line_height, int max_rows)
{
    const struct media_entry *e = media_at(app, p);
    if (!e || e->width < 1 || e->height < 1)
        return 1;
    int h = e->height;
    if (e->width > width)
        h = (int)((int64_t)h * width / e->width);
    int rows = (h + line_height - 1) / line_height + 1;
    if (rows > max_rows)
        rows = max_rows;
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
bool media_paste_image(struct app *app, const unsigned char *data, size_t length)
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
    char timestamp[32], name[256];
    strftime(timestamp, sizeof(timestamp), "%Y%m%d-%H%M%S", &tm);
    static unsigned sequence;
    bool written = false;
    for (unsigned attempt = 0; attempt < 16 && !written; ++attempt) {
        snprintf(name, sizeof(name), "assets/%s-%08x.png", timestamp,
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
            written = session_write_new(path, (const char *)data, bytes);
        free(path);
    }
    free(encoded);
    if (!written)
        return false;
    char markup[280];
    snprintf(markup, sizeof(markup), "![](%s)\n", name);
    struct editor *ed = &app->editor;
    size_t p = ed->cursor;
    editor_change_begin(ed);
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
    ed->cursor = at;
    editor_change_end(ed);
    if (ed->mode == INSERT)
        editor_change_begin(ed);
    app->dirty = true;
    app->follow_cursor = true;
    session_changed(app);
    media_sync(app);
    media_tick(app);
    return true;
}
void media_paste_text(struct app *app, const char *text, size_t length)
{
    if (!length || length > 16 * 1024 * 1024 || memchr(text, 0, length))
        return;
    char *normal = malloc(length + 1);
    if (!normal)
        return;
    size_t n = 0;
    for (size_t i = 0; i < length; ++i) {
        if (text[i] == '\r' && i + 1 < length && text[i + 1] == '\n')
            continue;
        normal[n++] = text[i] == '\r' ? '\n' : text[i];
    }
    normal[n] = 0;
    bool linewise = n && normal[n - 1] == '\n';
    editor_put(&app->editor, normal, n, linewise, app->editor.put_after,
               app->editor.put_count ? app->editor.put_count : 1);
    free(normal);
    app->dirty = true;
    app->follow_cursor = true;
    session_changed(app);
}
void media_import_file(struct app *app, const char *path)
{
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
    int error = posix_spawnp(&pid, "tesseract", &actions, NULL, arguments, environ);
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
        bool done = waitpid(m->job, &status, WNOHANG) != 0;
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
        if (app->editor.search[0])
            editor_search(&app->editor, app->editor.search_forward, 1);
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
            e->ocr_done = true;
            continue;
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
void media_close(struct app *app)
{
    struct media *m = app->media;
    if (!m)
        return;
    if (m->job) {
#ifdef _WIN32
        TerminateProcess(m->job, 0);
        WaitForSingleObject(m->job, 1000);
        CloseHandle(m->job);
#else
        kill(m->job, SIGTERM);
        waitpid(m->job, NULL, 0);
#endif
    }
    free(m->entries);
    free(m);
    app->media = NULL;
}
void media_highlight(struct app *app, const struct media_entry *e, uint32_t *pixels, int width,
                     int height, int stride, int x, int y, int draw_width, int draw_height)
{
    if (!app->editor.search_highlight || !app->editor.search[0] || e->width < 1 || e->height < 1)
        return;
    size_t n;
    char *tsv = sidecar(app, e, ".tsv", &n);
    if (!tsv)
        return;
    char *p = tsv;
    while (*p) {
        char *end = strchr(p, '\n');
        if (!end)
            end = p + strlen(p);
        char saved = *end;
        *end = 0;
        int level, page, block, paragraph, line, word, left, top, w, h;
        float confidence;
        int text_at = 0;
        if (sscanf(p, "%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%f\t%n", &level, &page, &block,
                   &paragraph, &line, &word, &left, &top, &w, &h, &confidence, &text_at) == 11 &&
            text_at > 0 && level == 5 &&
            contains(p + text_at, strlen(p + text_at), app->editor.search)) {
            int x0 = x + (int)((int64_t)left * draw_width / e->width),
                y0 = y + (int)((int64_t)top * draw_height / e->height),
                x1 = x + (int)((int64_t)(left + w) * draw_width / e->width),
                y1 = y + (int)((int64_t)(top + h) * draw_height / e->height);
            for (int py = y0; py < y1; ++py)
                for (int px = x0; px < x1; ++px)
                    if (px >= 0 && py >= 0 && px < width && py < height) {
                        uint32_t c = pixels[(size_t)py * stride + px];
                        pixels[(size_t)py * stride + px] =
                            0xff000000 | ((((c >> 16) & 255) + 255) / 2 << 16) |
                            ((((c >> 8) & 255) + 210) / 2 << 8) | ((c & 255) / 2);
                    }
        }
        *end = saved;
        if (!saved)
            break;
        p = end + 1;
    }
    free(tsv);
}
