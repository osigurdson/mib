#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L
#include "session.h"
#include "app.h"
#include "edit-internal.h"
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
#include <sys/stat.h>
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
    /* Like realpath, fail for a path that does not exist, so a page never saved gets no
     * remembered position. */
    wchar_t *wide = windows_path(path);
    if (!wide || GetFileAttributesW(wide) == INVALID_FILE_ATTRIBUTES) {
        free(wide);
        return NULL;
    }
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
        .view = app->editor.view,
        .colorscheme = app->editor.colorscheme,
        .maximized = app->maximized,
        .light = app->editor.light,
        .number = app->editor.number,
        .relativenumber = app->editor.relativenumber,
    };
}
static void apply_settings(struct app *app, const struct app_settings *s)
{
    app->width = app->restore_width = s->width;
    app->height = app->restore_height = s->height;
    app->font_size = s->font_size;
    app->editor.view = s->view;
    app->editor.colorscheme = s->colorscheme;
    app->maximized = s->maximized;
    app->editor.light = s->light;
    app->editor.number = s->number;
    app->editor.relativenumber = s->relativenumber;
}
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
        } else if (!strcmp(key, "background") &&
                   (!strcmp(value, "light") || !strcmp(value, "dark")))
            s->light = !strcmp(value, "light");
        else if (!strcmp(key, "view") || !strcmp(key, "render")) {
            /* render = raw/normal/polish is the old spelling of view. */
            static const char *const old[] = {"raw", "normal", "polish"};
            for (unsigned i = 0; i < VIEW_COUNT; ++i)
                if (!strcmp(value, editor_view_name((enum view_mode)i)) || !strcmp(value, old[i]))
                    s->view = (enum view_mode)i;
        } else if (!strcmp(key, "font_size") && numeric && number >= 8 && number <= 48)
            s->font_size = (unsigned)number;
        else if (!strcmp(key, "width") && numeric && number >= 200 && number <= 10000)
            s->width = (int)number;
        else if (!strcmp(key, "height") && numeric && number >= 100 && number <= 10000)
            s->height = (int)number;
        else if (!strcmp(key, "maximized") && (yes || no))
            s->maximized = yes;
        else if (!strcmp(key, "number") && (yes || no))
            s->number = yes;
        else if (!strcmp(key, "relativenumber") && (yes || no))
            s->relativenumber = yes;
    }
}
/* folder/name for a variable naming an absolute folder, or NULL. */
static char *from_variable(const char *variable, const char *name)
{
    char *base = platform_getenv(variable);
#ifndef _WIN32
    if (base && base[0] != '/') { /* XDG folders must be absolute */
        free(base);
        base = NULL;
    }
#endif
    char *path = base ? join(base, name) : NULL;
    free(base);
    return path;
}
char *session_config_directory(void)
{
#ifdef _WIN32
    return from_variable("APPDATA", "jot");
#else
    char *path = from_variable("XDG_CONFIG_HOME", "jot");
    return path ? path : from_variable("HOME", ".config/jot");
#endif
}
char *session_state_directory(void)
{
#ifdef _WIN32
    return from_variable("LOCALAPPDATA", "jot");
#else
    char *path = from_variable("XDG_STATE_HOME", "jot");
    return path ? path : from_variable("HOME", ".local/state/jot");
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
                         "colorscheme = %s\nbackground = %s\nview = %s\nfont_size = %u\n"
                         "width = %d\nheight = %d\nmaximized = %s\nnumber = %s\n"
                         "relativenumber = %s\n",
                         editor_colorscheme_name(s->colorscheme), s->light ? "light" : "dark",
                         editor_view_name(s->view), s->font_size, s->width, s->height,
                         s->maximized ? "true" : "false", s->number ? "true" : "false",
                         s->relativenumber ? "true" : "false");
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
/* Apply the shared settings file and restore the shown page's position. Closing writes back
 * the settings this window changed. */
static void load_settings(struct app *app)
{
    struct app_settings s = current_settings(app);
    char *text = read_settings();
    if (text)
        parse_settings(text, &s);
    free(text);
    apply_settings(app, &s);
    app->settings = s;
    app->settings_loaded = true;
    app->remember_positions = true;
    session_restore_position(&app->editor);
}
enum { POSITION_LIMIT = 200 };
static char *positions_file(void)
{
    char *folder = session_state_directory();
    char *path = folder ? join(folder, "positions") : NULL;
    free(folder);
    return path;
}
/* Parse one "line column top path" line; path points into line. */
static bool parse_position(char *line, size_t *row, size_t *column, size_t *top, char **path)
{
    unsigned long long r, c, t;
    int used = 0;
    if (sscanf(line, "%llu %llu %llu %n", &r, &c, &t, &used) != 3 || !used || !line[used])
        return false;
    *row = (size_t)r;
    *column = (size_t)c;
    *top = (size_t)t;
    *path = line + used;
    return true;
}
void session_restore_position(struct editor *ed)
{
    char *file = positions_file(), *key = ed->filename ? session_absolute(ed->filename) : NULL;
    size_t n;
    char *text = file && key ? session_read(file, 1 << 20, &n) : NULL;
    for (char *line = text; line && *line;) {
        char *end = strchr(line, '\n');
        if (end)
            *end = 0;
        size_t row, column, top;
        char *path;
        if (parse_position(line, &row, &column, &top, &path) && !strcmp(path, key)) {
            size_t start = editor_line_at(ed, row), stop = editor_line_end(ed, start);
            size_t cursor = column < stop - start ? start + column : stop;
            /* Land on a character's first byte if the file changed under the column. */
            while (cursor > start && ((unsigned char)ed->text[cursor] & 0xc0) == 0x80)
                --cursor;
            ed->cursor = cursor;
            ed->top = top <= row ? editor_line_at(ed, top) : start;
            editor_clamp(ed);
            break;
        }
        line = end ? end + 1 : NULL;
    }
    free(text);
    free(key);
    free(file);
}
struct position {
    char *path;
    size_t row, column, top;
};
static void add_position(struct position *list, unsigned *count, const char *filename,
                         bool modified, const char *text, size_t length, size_t cursor, size_t top)
{
    /* A document with unsaved changes keeps the position from its last save, so a position
     * always refers to text on disk even when the changes are discarded. */
    if (!filename || modified || *count >= PAGE_COUNT + 1)
        return;
    char *path = session_absolute(filename);
    if (!path)
        return;
    const struct editor view = {.text = (char *)text, .length = length};
    if (cursor > length)
        cursor = length;
    list[*count] = (struct position){path, editor_line_number(&view, cursor),
                                     cursor - editor_line_start(&view, cursor),
                                     editor_line_number(&view, top < length ? top : length)};
    ++*count;
}
void session_save_positions(struct app *app)
{
    if (!app->remember_positions)
        return;
    struct position fresh[PAGE_COUNT + 1];
    unsigned count = 0;
    const struct editor *ed = &app->editor;
    add_position(fresh, &count, ed->filename, ed->modified, ed->text, ed->length, ed->cursor,
                 ed->top);
    for (unsigned i = 0; i < PAGE_COUNT; ++i) {
        const struct page *p = &app->pages[i];
        if (p->loaded)
            add_position(fresh, &count, p->filename, p->modified, p->text, p->length, p->cursor,
                         p->top);
    }
    if (!count)
        return;
    /* Keep other files' entries, written by this or another window, after the fresh ones. */
    char *file = positions_file(), *folder = session_state_directory();
    size_t n = 0, size = 1;
    char *old = file ? session_read(file, 1 << 20, &n) : NULL;
    for (unsigned i = 0; i < count; ++i)
        size += strlen(fresh[i].path) + 3 * 21 + 1;
    char *out = malloc(size + (old ? n : 0));
    size_t used = 0;
    unsigned kept = count;
    for (unsigned i = 0; out && i < count; ++i)
        used += (size_t)sprintf(out + used, "%zu %zu %zu %s\n", fresh[i].row, fresh[i].column,
                                fresh[i].top, fresh[i].path);
    for (char *line = out ? old : NULL; line && *line && kept < POSITION_LIMIT;) {
        char *end = strchr(line, '\n');
        if (end)
            *end = 0;
        size_t length = strlen(line), row, column, top;
        memcpy(out + used, line, length);
        out[used + length] = 0;
        char *path = NULL;
        bool ok = parse_position(out + used, &row, &column, &top, &path);
        for (unsigned i = 0; ok && i < count; ++i)
            ok = strcmp(path, fresh[i].path) != 0;
        if (ok) {
            memcpy(out + used, line, length);
            out[used + length] = '\n';
            used += length + 1;
            ++kept;
        }
        line = end ? end + 1 : NULL;
    }
    char error[128];
    if (out && folder && session_mkdir(folder))
        platform_file_write_atomic(file, out, used, error, sizeof(error));
    free(out);
    free(old);
    free(folder);
    free(file);
    for (unsigned i = 0; i < count; ++i)
        free(fresh[i].path);
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
    KEEP(view)
    KEEP(colorscheme)
    KEEP(maximized)
    KEEP(light)
    KEEP(number)
    KEEP(relativenumber)
#undef KEEP
    if (changed)
        write_settings(&s);
    app->settings_loaded = false;
}
/* The notes folder: name is a path, or NULL or "" for $JOT_DIR, else ~/Documents/jot
 * (~/jot without Documents). */
static char *resolve(const char *name)
{
    char *home = platform_getenv(
#ifdef _WIN32
        "USERPROFILE"
#else
        "HOME"
#endif
    );
    const char *base = home ? home : ".";
    char *path, *env = NULL;
    if (!name || !*name)
        name = env = platform_getenv("JOT_DIR");
    if (!name) {
        char *docs = join(base, "Documents");
        path = join(docs && directory(docs) ? docs : base, "jot");
        free(docs);
    } else if (name[0] == '~' && (name[1] == '/' || name[1] == '\\' || !name[1]))
        path = name[1] ? join(base, name + 2) : join(base, "");
    else {
        path = malloc(strlen(name) + 1);
        if (path)
            strcpy(path, name);
    }
    free(home);
    free(env);
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
    /* Canonicalize after mkdir to merge relative/symlink aliases. */
#endif
    return path;
}
/* The state file holding the number of the page shown when Jot last closed. */
static char *page_file(void)
{
    char *folder = session_state_directory();
    char *path = folder ? join(folder, "page") : NULL;
    free(folder);
    return path;
}
static unsigned last_page(void)
{
    char *file = page_file();
    size_t n;
    char *text = file ? session_read(file, 16, &n) : NULL;
    unsigned page = text && text[0] >= '1' && text[0] <= '9' ? (unsigned)(text[0] - '1') : 0;
    free(text);
    free(file);
    return page < PAGE_COUNT ? page : 0;
}
static void save_page(const struct app *app)
{
    if (!app->root)
        return;
    char *file = page_file(), *folder = session_state_directory(), text[4], error[128];
    snprintf(text, sizeof(text), "%u\n", app->current + 1);
    if (file && folder && session_mkdir(folder))
        platform_file_write_atomic(file, text, strlen(text), error, sizeof(error));
    free(file);
    free(folder);
}
static bool private_folder(const char *path)
{
    if (!session_mkdir(path))
        return false;
#ifndef _WIN32
    if (chmod(path, 0700) != 0)
        return false;
#endif
    return true;
}
int session_open(struct app *app, const char *name)
{
    app->directory = resolve(name);
    if (!app->directory || !private_folder(app->directory))
        goto failed;
#ifndef _WIN32
    char *canonical = realpath(app->directory, NULL);
    if (!canonical)
        goto failed;
    free(app->directory);
    app->directory = canonical;
#endif
    app->root = malloc(strlen(app->directory) + 1);
    if (!app->root)
        goto failed;
    strcpy(app->root, app->directory);
    /* Show the page shown when Jot last closed, page 1 at first. */
    app->current = last_page();
    char number[4];
    snprintf(number, sizeof(number), "%u", app->current + 1);
    char *folder = session_path(app, number);
    if (!folder)
        goto failed;
    free(app->directory);
    app->directory = folder;
    char *assets = session_path(app, "assets");
    bool made = folder && private_folder(folder) && assets && private_folder(assets);
    free(assets);
    if (!made)
        goto failed;
    char *page = session_path(app, "jot.md");
    if (!page)
        goto failed;
    bool missing;
    char error[128];
    FILE *existing = platform_file_open_read(page, &missing, error, sizeof(error));
    if (existing)
        fclose(existing);
    editor_destroy(&app->editor);
    app->editor = (struct editor){0};
    bool ok = editor_open(&app->editor, page);
    free(page);
    if (!ok)
        return -1;
    app->editor.on_save = app_saved;
    app->editor.on_save_data = app;
    app->media_scanned = false;
    media_sync(app);
    app->editor.cursor = app->editor.length;
    load_settings(app);
    if (!existing && missing && !editor_save(&app->editor, app->editor.filename))
        return -1;
    return 1;
failed:
    snprintf(app->editor.message, sizeof(app->editor.message), "Could not open private page: %s",
             strerror(errno));
    return -1;
}
void session_focus(struct app *app, bool focused)
{
    if (!app->root)
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
    session_save_positions(app);
    save_page(app);
    free(app->directory);
    app->directory = NULL;
    free(app->root);
    app->root = NULL;
}
