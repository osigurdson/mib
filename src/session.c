#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#include "session.h"
#include "app.h"
#include "media.h"
#include "platform.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include "platform/windows/win32.h"
#include <direct.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

uint64_t session_now(void)
{
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (unsigned long)t.tv_nsec / 1000000;
#endif
}
static char *join(const char *base, const char *relative)
{
    size_t a = strlen(base), b = strlen(relative);
    char *p = malloc(a + b + 2);
    if (p) {
        memcpy(p, base, a);
        p[a] = '/';
        memcpy(p + a + 1, relative, b + 1);
    }
    return p;
}
char *session_path(const struct app *app, const char *relative)
{
    return app->directory ? join(app->directory, relative) : NULL;
}
static bool directory(const char *path)
{
#ifdef _WIN32
    wchar_t *w = windows_path(path);
    if (!w)
        return false;
    DWORD attrs = GetFileAttributesW(w);
    free(w);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}
bool session_mkdir(const char *path)
{
    char *copy = malloc(strlen(path) + 1);
    if (!copy)
        return false;
    strcpy(copy, path);
    bool ok = true;
    for (char *p = copy + 1;; ++p) {
        if (*p != '/' && *p != '\\' && *p)
            continue;
        char c = *p;
        *p = 0;
#ifdef _WIN32
        if (strlen(copy) > 2) {
            wchar_t *w = windows_path(copy);
            if (!w || (!CreateDirectoryW(w, NULL) && GetLastError() != ERROR_ALREADY_EXISTS))
                ok = false;
            free(w);
        }
#else
        if (mkdir(copy, 0700) != 0 && errno != EEXIST)
            ok = false;
#endif
        *p = c;
        if (!ok || !c)
            break;
    }
    free(copy);
    return ok && directory(path);
}
char *session_read(const char *path, size_t maximum, size_t *length)
{
    bool missing;
    char error[128];
    FILE *f = platform_file_open_read(path, &missing, error, sizeof(error));
    if (!f)
        return NULL;
    size_t n = 0, capacity = maximum < 1024 ? maximum : 1024;
    char *text = malloc(capacity + 1);
    if (!text) {
        fclose(f);
        return NULL;
    }
    bool ok = true;
    for (;;) {
        if (n == capacity) {
            if (n == maximum) {
                int c = fgetc(f);
                if (c != EOF)
                    ok = false;
                break;
            }
            size_t next = capacity * 2;
            if (next > maximum)
                next = maximum;
            char *p = realloc(text, next + 1);
            if (!p) {
                ok = false;
                break;
            }
            text = p;
            capacity = next;
        }
        size_t count = fread(text + n, 1, capacity - n, f);
        n += count;
        if (!count) {
            ok = !ferror(f);
            break;
        }
    }
    fclose(f);
    if (!ok) {
        free(text);
        return NULL;
    }
    text[n] = 0;
    if (length)
        *length = n;
    return text;
}
static bool write_new(const char *path, const char *data, size_t length, bool existing_ok)
{
#ifdef _WIN32
    wchar_t *w = windows_path(path);
    if (!w)
        return false;
    HANDLE file = CreateFileW(w, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    if (file == INVALID_HANDLE_VALUE)
        return existing_ok &&
               (GetLastError() == ERROR_FILE_EXISTS || GetLastError() == ERROR_ALREADY_EXISTS);
    DWORD written;
    bool ok = length <= MAXDWORD && WriteFile(file, data, (DWORD)length, &written, NULL) &&
              written == length;
    CloseHandle(file);
    return ok;
#else
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0)
        return existing_ok && errno == EEXIST;
    bool ok = true;
    size_t pos = 0;
    while (pos < length) {
        ssize_t n = write(fd, data + pos, length - pos);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            ok = false;
            break;
        }
        pos += (size_t)n;
    }
    if (close(fd) != 0)
        ok = false;
    return ok;
#endif
}
bool session_write_new(const char *path, const char *data, size_t length)
{
    return write_new(path, data, length, true);
}
bool session_write_exclusive(const char *path, const char *data, size_t length)
{
    return write_new(path, data, length, false);
}
char *session_absolute(const char *path)
{
#ifdef _WIN32
    wchar_t *wide = windows_path(path);
    if (!wide)
        return NULL;
    char *text = windows_utf8(wide);
    free(wide);
    return text;
#else
    return realpath(path, NULL);
#endif
}
static struct app_settings current_settings(const struct app *app)
{
    return (struct app_settings){
        .width = app->maximized ? app->restore_width : app->width,
        .height = app->maximized ? app->restore_height : app->height,
        .font_size = app->font_size,
        .render_mode = app->editor.render_mode,
        .colorscheme = app->editor.colorscheme,
        .maximized = app->maximized,
        .light = app->editor.light,
    };
}
static void apply_settings(struct app *app, const struct app_settings *s)
{
    app->width = app->restore_width = s->width;
    app->height = app->restore_height = s->height;
    app->font_size = s->font_size;
    app->editor.render_mode = s->render_mode;
    app->editor.colorscheme = s->colorscheme;
    app->maximized = s->maximized;
    app->editor.light = s->light;
}
static const char *const render_names[] = {"raw", "normal", "polish"};
/* Lines of "key = value"; blank lines, # comments and bad values are ignored. */
static void parse_settings(const char *text, struct app_settings *s)
{
    while (*text) {
        const char *end = strchr(text, '\n');
        if (!end)
            end = text + strlen(text);
        char line[160] = "";
        size_t n = (size_t)(end - text);
        if (n < sizeof(line)) {
            memcpy(line, text, n);
            line[n] = 0;
        }
        text = *end ? end + 1 : end;
        char key[32], value[64];
        if (line[0] == '#' || sscanf(line, " %31[a-z_] = %63s", key, value) != 2)
            continue;
        char *rest;
        unsigned long number = strtoul(value, &rest, 10);
        bool numeric = rest != value && !*rest;
        bool yes = !strcmp(value, "true") || !strcmp(value, "1"),
             no = !strcmp(value, "false") || !strcmp(value, "0");
        if (!strcmp(key, "colorscheme")) {
            for (unsigned i = 0; i < COLOR_COUNT; ++i)
                if (!strcmp(value, editor_colorscheme_name((enum color_scheme)i)))
                    s->colorscheme = (enum color_scheme)i;
        } else if (!strcmp(key, "background") && (!strcmp(value, "light") || !strcmp(value, "dark")))
            s->light = !strcmp(value, "light");
        else if (!strcmp(key, "render")) {
            for (unsigned i = RENDER_RAW; i <= RENDER_POLISH; ++i)
                if (!strcmp(value, render_names[i]))
                    s->render_mode = (enum render_mode)i;
        } else if (!strcmp(key, "font_size") && numeric && number >= 8 && number <= 48)
            s->font_size = (unsigned)number;
        else if (!strcmp(key, "width") && numeric && number >= 200 && number <= 10000)
            s->width = (int)number;
        else if (!strcmp(key, "height") && numeric && number >= 100 && number <= 10000)
            s->height = (int)number;
        else if (!strcmp(key, "maximized") && (yes || no))
            s->maximized = yes;
    }
}
/* The old per-page format: "width height font numbers render maximized light scheme". */
static bool parse_legacy(const char *text, struct app_settings *s)
{
    unsigned w, h, font, numbers, render, maximized = 0, light = 0;
    char scheme[32] = "jot";
    if (sscanf(text, "%u %u %u %u %u %u %u %31s", &w, &h, &font, &numbers, &render, &maximized,
               &light, scheme) < 5)
        return false;
    if (w >= 200 && w <= 10000)
        s->width = (int)w;
    if (h >= 100 && h <= 10000)
        s->height = (int)h;
    if (font >= 8 && font <= 48)
        s->font_size = font;
    s->render_mode = render <= RENDER_POLISH ? (enum render_mode)render : RENDER_NORMAL;
    s->colorscheme = COLOR_JOT;
    for (unsigned i = 0; i < COLOR_COUNT; ++i)
        if (!strcmp(scheme, editor_colorscheme_name((enum color_scheme)i)))
            s->colorscheme = (enum color_scheme)i;
    s->light = light != 0;
    s->maximized = maximized != 0;
    return true;
}
char *session_config_directory(void)
{
#ifdef _WIN32
    const char *base = getenv("APPDATA");
    return base && *base ? join(base, "jot") : NULL;
#else
    const char *base = getenv("XDG_CONFIG_HOME");
    if (base && base[0] == '/')
        return join(base, "jot");
    base = getenv("HOME");
    return base && *base ? join(base, ".config/jot") : NULL;
#endif
}
static bool write_settings(const struct app_settings *s)
{
    char *folder = session_config_directory();
    char *path = folder ? join(folder, "config") : NULL;
    bool ok = path && session_mkdir(folder);
    if (ok) {
        char text[512], error[128];
        int n = snprintf(text, sizeof(text),
                         "# Jot settings, shared by every page. Jot updates this file when a\n"
                         "# window closes, keeping changes made here to other settings.\n"
                         "colorscheme = %s\nbackground = %s\nrender = %s\nfont_size = %u\n"
                         "width = %d\nheight = %d\nmaximized = %s\n",
                         editor_colorscheme_name(s->colorscheme), s->light ? "light" : "dark",
                         render_names[s->render_mode], s->font_size, s->width, s->height,
                         s->maximized ? "true" : "false");
        ok = n > 0 && (size_t)n < sizeof(text) &&
             platform_file_write_atomic(path, text, (size_t)n, error, sizeof(error));
    }
    free(folder);
    free(path);
    return ok;
}
static char *read_settings(void)
{
    char *folder = session_config_directory();
    char *path = folder ? join(folder, "config") : NULL;
    size_t n;
    char *text = path ? session_read(path, 16384, &n) : NULL;
    free(folder);
    free(path);
    return text;
}
void session_load_settings(struct app *app)
{
    struct app_settings s = current_settings(app);
    char *text = read_settings();
    if (text)
        parse_settings(text, &s);
    else if (app->directory) {
        char *legacy = session_path(app, ".tinyvi-state");
        size_t n;
        char *old = legacy ? session_read(legacy, 256, &n) : NULL;
        if (old && parse_legacy(old, &s))
            write_settings(&s);
        free(old);
        free(legacy);
    }
    free(text);
    apply_settings(app, &s);
    app->settings = s;
    app->settings_loaded = true;
}
/* Another window may have changed other settings since this one opened, so start from the
 * file and apply only what changed here. */
static void save_settings(struct app *app)
{
    if (!app->settings_loaded)
        return;
    struct app_settings now = current_settings(app), was = app->settings, s = was;
    char *text = read_settings();
    if (text)
        parse_settings(text, &s);
    bool changed = !text;
    free(text);
#define KEEP(field)                                                                                \
    if (now.field != was.field) {                                                                  \
        s.field = now.field;                                                                       \
        changed = true;                                                                            \
    }
    KEEP(width)
    KEEP(height)
    KEEP(font_size)
    KEEP(render_mode)
    KEEP(colorscheme)
    KEEP(maximized)
    KEEP(light)
#undef KEEP
    if (changed)
        write_settings(&s);
    app->settings_loaded = false;
}
static const char agents[] =
    "# jot scratch page\n\n"
    "This folder contains quick working notes and screenshots. Read and answer by default.\n"
    "Do not tidy, reorganize or rewrite jot.md unless explicitly asked. Put summaries in chat\n"
    "or a separate file. When asked to edit, append and keep the existing style.\n\n"
    "- jot.md is the running page, usually newest notes at the bottom.\n"
    "- room2.md to room9.md are further scratch rooms (jot.md is room 1); they share assets/.\n"
    "- ![](assets/name.png) lines are screenshots; retain them on their own lines.\n"
    "- assets/name.png.txt is OCR text; read it before opening the image. OCR can be wrong.\n"
    "- assets/name.png.tsv contains OCR word positions used for highlighting.\n"
    "- Image names begin with the paste timestamp (YYYYMMDD-HHMMSS).\n\n"
    "Save edits explicitly with :w or Ctrl+S. After an external edit, switch to the window\n"
    "to reload if there are no local edits. Never edit concurrently with unsaved typing.\n";
static char *resolve(const char *name)
{
    const char *home = getenv(
#ifdef _WIN32
        "USERPROFILE"
#else
        "HOME"
#endif
    );
    if (!home)
        home = ".";
    if (!name || !*name) {
        const char *env = getenv("JOT_DIR");
        if (env && *env)
            name = env;
        else
            name = "default";
    }
    char *path;
    if (strchr(name, '/') || strchr(name, '\\') || !strcmp(name, ".") || !strcmp(name, "..") ||
        name[0] == '~' || (name[0] && name[1] == ':')) {
        if (name[0] == '~' && (name[1] == '/' || !name[1]))
            path = name[1] ? join(home, name + 2) : join(home, "");
        else {
            path = malloc(strlen(name) + 1);
            if (path)
                strcpy(path, name);
        }
    } else {
        char *docs = join(home, "Documents");
        char *root = join(docs && directory(docs) ? docs : home, "jot");
        free(docs);
        path = root ? join(root, name) : NULL;
        free(root);
    }
    if (!path)
        return NULL;
#ifdef _WIN32
    wchar_t *w = windows_path(path);
    free(path);
    if (!w)
        return NULL;
    path = windows_utf8(!wcsncmp(w, L"\\\\?\\UNC\\", 8) ? w : w + 4);
    free(w);
#else
    /* Canonicalize after mkdir to merge relative/symlink aliases for ownership. */
#endif
    return path;
}
#ifdef _WIN32
struct owner_window {
    const wchar_t *property;
    HWND handle;
};
static BOOL CALLBACK find_owner(HWND window, LPARAM value)
{
    struct owner_window *owner = (void *)value;
    if (GetPropW(window, owner->property)) {
        owner->handle = window;
        return FALSE;
    }
    return TRUE;
}
#endif
static int ownership(struct app *app)
{
#ifdef _WIN32
    uint64_t hash = 14695981039346656037ull;
    for (const unsigned char *p = (const unsigned char *)app->directory; *p; ++p) {
        unsigned char c = *p;
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
        if (c == '\\')
            c = '/';
        hash = (hash ^ c) * 1099511628211ull;
    }
    wchar_t name[64];
    swprintf(name, 64, L"Local\\TinyViPage-%016llx", (unsigned long long)hash);
    HANDLE mutex = CreateMutexW(NULL, FALSE, name);
    if (!mutex)
        return -1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        struct owner_window owner = {.property = name};
        EnumWindows(find_owner, (LPARAM)&owner);
        HWND window = owner.handle;
        if (window) {
            ShowWindow(window, SW_RESTORE);
            SetForegroundWindow(window);
        }
        CloseHandle(mutex);
        return 0;
    }
    app->owner_handle = mutex;
    app->page_id = hash;
    return 1;
#else
    char *lock = session_path(app, ".tinyvi.lock");
    if (!lock)
        return -1;
    int fd = open(lock, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    free(lock);
    if (fd < 0)
        return -1;
    /* A short stable abstract socket name avoids sockaddr_un path limits. */
    uint64_t hash = 14695981039346656037ull;
    for (const unsigned char *p = (const unsigned char *)app->directory; *p; ++p)
        hash = (hash ^ *p) * 1099511628211ull;
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    snprintf(addr.sun_path + 1, sizeof(addr.sun_path) - 1, "tinyvi-%lu-%016llx",
             (unsigned long)getuid(), (unsigned long long)hash);
    int sock = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const char *token = getenv("XDG_ACTIVATION_TOKEN");
        if (!token)
            token = "";
        if (sock >= 0) {
            sendto(sock, token, strlen(token) + 1, 0, (struct sockaddr *)&addr, sizeof(addr));
            close(sock);
        }
        close(fd);
        return 0;
    }
    if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        if (sock >= 0)
            close(sock);
        close(fd);
        return -1;
    }
    app->owner_fd = fd;
    app->activation_fd = sock;
    return 1;
#endif
}
int session_open(struct app *app, const char *name)
{
    app->owner_fd = app->activation_fd = -1;
    app->directory = resolve(name);
    if (!app->directory)
        goto failed;
    if (!session_mkdir(app->directory))
        goto failed;
#ifndef _WIN32
    char *canonical = realpath(app->directory, NULL);
    if (!canonical)
        goto failed;
    free(app->directory);
    app->directory = canonical;
    if (chmod(app->directory, 0700) != 0)
        goto failed;
#endif
    char *assets = session_path(app, "assets");
    bool made = assets && session_mkdir(assets);
#ifndef _WIN32
    if (made && chmod(assets, 0700) != 0)
        made = false;
#endif
    free(assets);
    if (!made)
        goto failed;
    int owned = ownership(app);
    if (owned <= 0) {
        if (owned < 0)
            goto failed;
        return 0;
    }
    char *page = session_path(app, "jot.md");
    if (!page)
        goto failed;
    bool missing;
    char error[128];
    FILE *existing = platform_file_open_read(page, &missing, error, sizeof(error));
    if (existing)
        fclose(existing);
    if (!existing && missing) {
        char *a = session_path(app, "AGENTS.md"), *c = session_path(app, "CLAUDE.md");
        bool ok = a && c && session_write_new(a, agents, sizeof(agents) - 1);
#ifndef _WIN32
        if (ok && symlink("AGENTS.md", c) != 0 && errno != EEXIST)
            ok = session_write_new(c, agents, sizeof(agents) - 1);
#else
        if (ok)
            ok = session_write_new(c, agents, sizeof(agents) - 1);
#endif
        free(a);
        free(c);
        if (!ok) {
            free(page);
            goto failed;
        }
    }
    editor_destroy(&app->editor);
    app->editor = (struct editor){0};
    bool ok = editor_open(&app->editor, page);
    free(page);
    if (!ok)
        return -1;
    app->editor.on_save = media_saved;
    app->editor.on_save_data = app;
    app->media_scanned = false;
    media_sync(app);
    app->scratch = true;
    app->editor.cursor = app->editor.length;
    session_load_settings(app);
    if (missing && !editor_save(&app->editor, app->editor.filename))
        return -1;
    return 1;
failed:
    snprintf(app->editor.message, sizeof(app->editor.message), "Could not open private page: %s",
             strerror(errno));
    return -1;
}
void session_focus(struct app *app, bool focused)
{
    if (!app->scratch)
        return;
    if (!focused || app->editor.modified)
        return;
    struct editor check = {0};
    if (editor_open(&check, app->editor.filename)) {
        size_t n = check.length;
        if (n != app->editor.length || memcmp(check.text, app->editor.text, n)) {
            editor_reload(&app->editor);
            app->dirty = true;
        }
    }
    editor_destroy(&check);
}
void session_close(struct app *app)
{
    save_settings(app);
#ifdef _WIN32
    if (app->owner_handle)
        CloseHandle(app->owner_handle);
#else
    if (app->owner_fd >= 0)
        close(app->owner_fd);
    if (app->activation_fd >= 0)
        close(app->activation_fd);
#endif
    free(app->directory);
    app->directory = NULL;
}
