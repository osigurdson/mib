#define _POSIX_C_SOURCE 200809L
#include "app.h"
#include "platform.h"
#include "session.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include "platform/windows/win32.h"
#else
#include <sys/stat.h>
#include <unistd.h>
#endif
static char *config_file(void)
{
    char *folder = session_config_directory();
    assert(folder);
    char *path = malloc(strlen(folder) + 8);
    assert(path);
    sprintf(path, "%s/config", folder);
    free(folder);
    return path;
}
static void remove_file(const char *path)
{
#ifdef _WIN32
    wchar_t *w = windows_path(path);
    DeleteFileW(w);
    free(w);
#else
    unlink(path);
#endif
}
static void remove_folder(const char *path)
{
#ifdef _WIN32
    wchar_t *w = windows_path(path);
    RemoveDirectoryW(w);
    free(w);
#else
    rmdir(path);
#endif
}
static void type(struct app *app, const char *keys)
{
    for (const char *p = keys; *p; ++p) {
        char c[2] = {*p, 0};
        app_input(app, *p == '\n' ? KEY_ENTER : *p == 27 ? KEY_ESCAPE : KEY_TEXT, c);
    }
}
static bool exists(const char *directory, const char *name)
{
    char path[1300];
    snprintf(path, sizeof(path), "%s/%s", directory, name);
    size_t n;
    char *text = session_read(path, 1 << 20, &n);
    free(text);
    return text != NULL;
}
static void write_room(const char *directory, const char *name, const char *text)
{
    char path[1300], error[128];
    snprintf(path, sizeof(path), "%s/%s", directory, name);
    char *slash = strrchr(path, '/');
    *slash = 0;
    assert(session_mkdir(path));
    *slash = '/';
    assert(platform_file_write_atomic(path, text, strlen(text), error, sizeof(error)));
}
static void rooms(const char *parent)
{
    char directory[1100], heading[64];
    snprintf(directory, sizeof(directory), "%s/rooms", parent);
    struct app app;
    assert(app_open(&app, NULL) && session_open(&app, directory) == 1);
    editor_command(&app.editor, "set raw");
    type(&app, "ione\ntwo\x1b");
    assert(app.room == 0 && strstr(app.editor.filename, "jot.md"));
    /* Alt+digit only switches from normal mode. */
    type(&app, "a");
    app_input(&app, KEY_ROOM, "3");
    assert(app.room == 0 && app.editor.mode == INSERT);
    type(&app, "\x1b");
    size_t cursor = app.editor.cursor;
    app.editor.top = 4;
    app.follow_cursor = false;
    app_input(&app, KEY_ROOM, "3");
    assert(app.room == 2 && app.editor.length == 0 && strstr(app.editor.filename, "room3/jot.md"));
    type(&app, "i# Groceries #\nmilk\x1b");
    /* Each room keeps its own text, cursor, scroll position and undo. */
    app_input(&app, KEY_ROOM, "1");
    assert(app.room == 0 && !strcmp(app.editor.text, "one\ntwo") && app.editor.modified);
    assert(app.editor.cursor == cursor && app.editor.top == 4 && !app.follow_cursor);
    type(&app, "u");
    assert(!strcmp(app.editor.text, ""));
    app_input(&app, KEY_CTRL_R, "");
    assert(!strcmp(app.editor.text, "one\ntwo"));
    /* Quitting protects every room. */
    app_input(&app, KEY_CTRL_S, "");
    assert(!app.editor.modified);
    type(&app, ":q\n");
    assert(app.running && strstr(app.editor.message, "room 3"));
    app_request_close(&app);
    assert(app.running && strstr(app.editor.message, "room 3"));
    /* The map labels rooms by their first H1, skipping fenced code. */
    write_room(directory, "room4/jot.md", "intro\n```\n# not this\n```\n#nospace\n# Real\n# Later\n");
    app_room_heading(&app, 2, heading, sizeof(heading));
    assert(!strcmp(heading, "Groceries"));
    app_room_heading(&app, 3, heading, sizeof(heading));
    assert(!strcmp(heading, "Real"));
    app_room_heading(&app, 0, heading, sizeof(heading));
    assert(!heading[0]);
    app_room_heading(&app, 4, heading, sizeof(heading));
    assert(!heading[0] && !exists(directory, "room5/jot.md"));
    type(&app, ":map\n");
    assert(app.room_map);
    uint32_t pixels[640 * 480];
    assert(app_paint(&app, pixels, 640, 480, 640));
    assert(app.map_width > 0 && app.map_width == app.map_height);
    assert(app_paint(&app, pixels, 120, 90, 120)); /* too small for keys: draws none */
    type(&app, "x");
    assert(app.room_map && !strcmp(app.editor.text, "one\ntwo"));
    type(&app, "3");
    assert(!app.room_map && app.room == 2 && strstr(app.editor.text, "milk"));
    type(&app, ":map\n\x1b");
    assert(!app.room_map && app.room == 2);
    /* Arrows or hjkl move the selection from the shown room, stopping at the edges, and
     * Enter shows the selected room. */
    type(&app, ":map\n");
    assert(app.room_map && app.map_choice == 2);
    app_input(&app, KEY_RIGHT, "");
    assert(app.map_choice == 2);
    app_input(&app, KEY_UP, "");
    app_input(&app, KEY_UP, "");
    app_input(&app, KEY_UP, "");
    assert(app.map_choice == 8 && app.room == 2 && app.room_map);
    app_input(&app, KEY_LEFT, "");
    app_input(&app, KEY_DOWN, "");
    assert(app.map_choice == 4);
    type(&app, "hjkl");
    assert(app.map_choice == 4 && app.room == 2 && strstr(app.editor.text, "milk"));
    type(&app, "hj");
    assert(app.map_choice == 0);
    app_input(&app, KEY_ENTER, "");
    assert(!app.room_map && app.room == 0 && !strcmp(app.editor.text, "one\ntwo"));
    app_input(&app, KEY_ROOM, "3");
    /* Clicking a cell shows that room; dragging on doesn't start a selection there. */
    type(&app, ":map\n");
    assert(app_paint(&app, pixels, 640, 480, 640));
    app_pointer(&app, app.map_x + app.map_width + app.map_gap + app.map_width / 2,
                app.map_y + app.map_height + app.map_gap + 2, false);
    assert(!app.room_map && app.room == 4);
    app_pointer(&app, 10, 10, true);
    assert(app.editor.mode == NORMAL);
    app_pointer_release(&app);
    /* Each room has its own assets: the same name in two rooms is two files, and saving a
     * room cleans up only its own. */
    char assets[1200], room_assets[1200];
    snprintf(assets, sizeof(assets), "%s/assets", directory);
    snprintf(room_assets, sizeof(room_assets), "%s/room3/assets", directory);
    write_room(assets, "a.png", "png");
    write_room(room_assets, "a.png", "png");
    app_input(&app, KEY_ROOM, "3");
    char *image = session_path(&app, "assets/a.png");
    assert(image && strstr(image, "room3"));
    free(image);
    type(&app, "Go![](assets/a.png)\x1b:w\n");
    app_input(&app, KEY_ROOM, "1");
    image = session_path(&app, "assets/a.png");
    assert(image && !strstr(image, "room3"));
    free(image);
    type(&app, "Go![](assets/a.png)\x1b");
    assert(app_paint(&app, pixels, 640, 480, 640));
    type(&app, "dd:w\n");
    assert(!exists(assets, "a.png") && exists(room_assets, "a.png"));
    app_input(&app, KEY_ROOM, "3");
    type(&app, "dd:w\n");
    assert(!strstr(app.editor.text, "a.png") && !exists(room_assets, "a.png"));
    /* A hidden room picks up outside edits when shown again, unless it has its own. */
    app_input(&app, KEY_ROOM, "4");
    write_room(directory, "room4/jot.md", "# Changed\n");
    app_input(&app, KEY_ROOM, "1");
    app_input(&app, KEY_ROOM, "4");
    assert(!strcmp(app.editor.text, "# Changed\n"));
    app_close(&app);
    /* Saved rooms come back after a restart; unsaved, untouched rooms leave no file. */
    assert(app_open(&app, NULL) && session_open(&app, directory) == 1);
    assert(app.room == 0 && !strcmp(app.editor.text, "one\ntwo"));
    app_input(&app, KEY_ROOM, "3");
    assert(!strcmp(app.editor.text, "# Groceries #\nmilk") &&
           app.editor.cursor == app.editor.length && app.follow_cursor);
    assert(!exists(directory, "room5/jot.md") && !exists(directory, "room2/jot.md"));
    app_close(&app);
    /* Explicit files have no rooms. */
    char file[1200];
    snprintf(file, sizeof(file), "%s/note.txt", directory);
    assert(app_open(&app, file));
    app_input(&app, KEY_ROOM, "2");
    assert(app.room == 0 && strstr(app.editor.message, "only available in pages"));
    type(&app, ":map\n");
    assert(!app.room_map && strstr(app.editor.message, "only available in pages"));
    app_close(&app);
    for (unsigned room = 2; room <= 9; ++room)
        for (unsigned i = 0; i < 3; ++i) {
            char path[1300];
            snprintf(path, sizeof(path), "%s/room%u%s", directory, room,
                     i == 0 ? "/jot.md" : i == 1 ? "/assets" : "");
            remove_file(path);
            remove_folder(path);
        }
    const char *files[] = {"jot.md", "AGENTS.md", "CLAUDE.md", ".tinyvi.lock", "assets", NULL};
    for (unsigned i = 0; files[i]; ++i) {
        char path[1300];
        snprintf(path, sizeof(path), "%s/%s", directory, files[i]);
        remove_file(path);
        remove_folder(path);
    }
    remove_folder(directory);
}
int main(void)
{
    char directory[1024], config[1100];
#ifdef _WIN32
    char temp[MAX_PATH];
    assert(GetTempPathA(MAX_PATH, temp));
    snprintf(directory, sizeof(directory), "%stinyvi-session-%lu", temp,
             (unsigned long)GetCurrentProcessId());
    snprintf(config, sizeof(config), "%s-config", directory);
    SetEnvironmentVariableA("APPDATA", config);
    char setting[1200];
    snprintf(setting, sizeof(setting), "APPDATA=%s", config);
    _putenv(setting);
#else
    snprintf(directory, sizeof(directory), "/tmp/tinyvi-session-XXXXXX");
    assert(mkdtemp(directory));
    snprintf(config, sizeof(config), "%s-config", directory);
    setenv("XDG_CONFIG_HOME", config, 1);
#endif
    char *settings = config_file();
    struct app app;
    assert(app_open(&app, NULL));
    assert(session_open(&app, directory) == 1);
    struct app second;
    assert(app_open(&second, NULL));
    assert(session_open(&second, directory) == 0);
    app_close(&second);
    char *page = session_path(&app, "jot.md"), *agents = session_path(&app, "AGENTS.md");
    size_t n;
    char *text = session_read(agents, 8192, &n);
    assert(text && strstr(text, "OCR"));
    free(text);
#ifndef _WIN32
    struct stat st;
    assert(!stat(directory, &st) && (st.st_mode & 0777) == 0700);
    assert(!stat(page, &st) && (st.st_mode & 0777) == 0600);
#endif
    app_input(&app, KEY_TEXT, "i");
    app_input(&app, KEY_TEXT, "hello");
    assert(app.editor.modified);
    session_focus(&app, false);
    text = session_read(page, 8192, &n);
    assert(text && !strcmp(text, ""));
    free(text);
    assert(app.editor.modified);
    app_input(&app, KEY_ESCAPE, "");
    app_input(&app, KEY_TEXT, ":");
    app_input(&app, KEY_TEXT, "q");
    app_input(&app, KEY_ENTER, "");
    assert(app.running && app.editor.modified && strstr(app.editor.message, "Unsaved"));
    app_input(&app, KEY_CTRL_Q, "");
    assert(app.running && app.editor.modified && strstr(app.editor.message, "Unsaved"));
    app_request_close(&app);
    assert(app.running && app.editor.modified && strstr(app.editor.message, "Unsaved"));
    app_input(&app, KEY_CTRL_S, "");
    assert(!app.editor.modified);
    text = session_read(page, 8192, &n);
    assert(text && !strcmp(text, "hello"));
    free(text);
    app_input(&app, KEY_TEXT, "i");
    assert(platform_file_write_atomic(page, "external\n", 9, app.editor.message,
                                      sizeof(app.editor.message)));
    session_focus(&app, true);
    assert(!strcmp(app.editor.text, "external\n"));
    app_input(&app, KEY_TEXT, "local");
    assert(app.editor.modified);
    assert(platform_file_write_atomic(page, "another\n", 8, app.editor.message,
                                      sizeof(app.editor.message)));
    session_focus(&app, true);
    assert(strstr(app.editor.text, "local"));
    session_focus(&app, false);
    assert(app.editor.modified);
    text = session_read(page, 8192, &n);
    assert(text && !strcmp(text, "another\n"));
    free(text);
    app.font_size = 23;
    app.width = 1400;
    app.maximized = true;
    app.restore_width = 900;
    app.restore_height = 600;
    app_close(&app);
    /* Settings go to the shared config file, not the page folder. */
    text = session_read(settings, 4096, &n);
    assert(text && strstr(text, "font_size = 23\n") && strstr(text, "width = 900\n") &&
           strstr(text, "maximized = true\n"));
    free(text);
    char *state = session_path(&(struct app){.directory = directory}, ".tinyvi-state");
    assert(state && !session_read(state, 256, &n));
    assert(app_open(&app, NULL));
    assert(session_open(&app, directory) == 1);
    assert(app.font_size == 23 && app.width == 900 && app.height == 600 && app.maximized);
    app_close(&app);
    /* Without a config file, a page's old settings seed it once. A legacy preference
     * disabling line numbers must no longer hide the gutter. */
    remove_file(settings);
    const char *legacy = "900 600 23 0 1 1\n";
    char error[128];
    assert(platform_file_write_atomic(state, legacy, strlen(legacy), error, sizeof(error)));
    assert(app_open(&app, NULL));
    assert(session_open(&app, directory) == 1);
    text = session_read(settings, 4096, &n);
    assert(text && strstr(text, "render = normal\n"));
    free(text);
    uint32_t pixels[320 * 200];
    assert(app_paint(&app, pixels, 320, 200, 320));
    assert(app.gutter == 6 * app.cell);
    assert(app.font_size == 23 && app.width == 900 &&
           app.height == 600 && app.maximized);
    assert(!strcmp(app.editor.text, "another\n")); /* close did not save local edits */
    assert(!app.editor.light && app.editor.colorscheme == COLOR_JOT); /* legacy preferences */
    editor_command(&app.editor, "set background=light");
    assert(app.editor.light && !app.editor.modified);
    app_close(&app);
    assert(app_open(&app, NULL));
    assert(session_open(&app, directory) == 1);
    assert(app.editor.light);
    assert(editor_reload(&app.editor) && app.editor.light);
    app_input(&app, KEY_TEXT, "i");
    app_input(&app, KEY_TEXT, "saved");
    app_input(&app, KEY_ESCAPE, "");
    app_input(&app, KEY_TEXT, ":");
    app_input(&app, KEY_TEXT, "w");
    app_input(&app, KEY_ENTER, "");
    assert(!app.editor.modified && app.running);
    text = session_read(page, 8192, &n);
    assert(text && strstr(text, "saved"));
    free(text);
    app_input(&app, KEY_TEXT, "i");
    app_input(&app, KEY_TEXT, "discarded");
    app_input(&app, KEY_ESCAPE, "");
    app_input(&app, KEY_TEXT, ":");
    app_input(&app, KEY_TEXT, "q!");
    app_input(&app, KEY_ENTER, "");
    assert(!app.running);
    app_close(&app);
    text = session_read(page, 8192, &n);
    assert(text && strstr(text, "saved") && !strstr(text, "discarded"));
    free(text);
    assert(app_open(&app, NULL));
    assert(session_open(&app, directory) == 1 && app.editor.light);
    editor_command(&app.editor, "set background=dark");
    app_close(&app);
    assert(app_open(&app, NULL));
    assert(session_open(&app, directory) == 1 && !app.editor.light);
    app_close(&app);
    const char *mode_commands[] = {"set raw", "set normal", "set polish"};
    for (unsigned mode = RENDER_RAW; mode <= RENDER_POLISH; ++mode) {
        assert(app_open(&app, NULL));
        assert(session_open(&app, directory) == 1);
        editor_command(&app.editor, mode_commands[mode]);
        app_close(&app);
        assert(app_open(&app, NULL));
        assert(session_open(&app, directory) == 1 && app.editor.render_mode == (enum render_mode)mode);
        assert(editor_reload(&app.editor) && app.editor.render_mode == (enum render_mode)mode);
        app_close(&app);
    }
    for (unsigned scheme = 0; scheme < COLOR_COUNT; ++scheme) {
        for (unsigned light = 0; light < 2; ++light) {
            assert(app_open(&app, NULL));
            assert(session_open(&app, directory) == 1);
            assert(editor_colorscheme(&app.editor, editor_colorscheme_name((enum color_scheme)scheme)));
            editor_command(&app.editor, light ? "set background=light" : "set background=dark");
            app_close(&app);
            assert(app_open(&app, NULL));
            assert(session_open(&app, directory) == 1);
            assert(app.editor.colorscheme == (enum color_scheme)scheme && app.editor.light == (bool)light);
            assert(editor_reload(&app.editor) && app.editor.colorscheme == (enum color_scheme)scheme);
            app_close(&app);
        }
    }
    /* Removed palettes do not get reinterpreted as a different enum value. */
    const char *retired[] = {"default", "desert", "evening", "peachpuff"};
    for (unsigned i = 0; i < sizeof(retired) / sizeof(retired[0]); ++i) {
        char legacy_state[128];
        snprintf(legacy_state, sizeof(legacy_state), "800 600 18 1 1 0 1 %s\n", retired[i]);
        remove_file(settings);
        assert(platform_file_write_atomic(state, legacy_state, strlen(legacy_state), error, sizeof(error)));
        assert(app_open(&app, NULL));
        assert(session_open(&app, directory) == 1);
        assert(app.editor.colorscheme == COLOR_JOT && app.editor.light);
        app_close(&app);
        snprintf(legacy_state, sizeof(legacy_state), "colorscheme = %s\nbackground = light\n",
                 retired[i]);
        assert(platform_file_write_atomic(settings, legacy_state, strlen(legacy_state), error,
                                          sizeof(error)));
        assert(app_open(&app, NULL));
        assert(session_open(&app, directory) == 1);
        assert(app.editor.colorscheme == COLOR_JOT && app.editor.light);
        app_close(&app);
    }

    /* Every page shares one set of settings, and the old page file is no longer read. */
    const char *shared = "# mine\ncolorscheme = kanagawa\nbackground = dark\nrender = polish\n"
                         "font_size = 20\nwidth = 1000\nheight = 700\nmaximized = false\n"
                         "bogus = 1\nwidth = tiny\n";
    assert(platform_file_write_atomic(settings, shared, strlen(shared), error, sizeof(error)));
    char other[1100];
    snprintf(other, sizeof(other), "%s/other", directory);
    struct app a, b;
    assert(app_open(&a, NULL) && session_open(&a, directory) == 1);
    assert(app_open(&b, NULL) && session_open(&b, other) == 1);
    for (struct app *p = &a; p; p = p == &a ? &b : NULL)
        assert(p->editor.colorscheme == COLOR_KANAGAWA && !p->editor.light &&
               p->editor.render_mode == RENDER_POLISH && p->font_size == 20 && p->width == 1000 &&
               p->height == 700 && !p->maximized);
    /* Closing without changes leaves a hand-edited file alone. */
    app_close(&b);
    text = session_read(settings, 4096, &n);
    assert(text && !strcmp(text, shared));
    free(text);
    /* Windows open at the same time keep each other's changes. */
    assert(app_open(&b, NULL) && session_open(&b, other) == 1);
    editor_command(&a.editor, "colorscheme everforest");
    editor_command(&b.editor, "set background=light");
    app_close(&a);
    app_close(&b);
    assert(app_open(&a, NULL) && session_open(&a, other) == 1);
    assert(a.editor.colorscheme == COLOR_EVERFOREST && a.editor.light &&
           a.editor.render_mode == RENDER_POLISH);
    app_close(&a);
    /* Explicit files use the same settings. */
    char file[1200];
    snprintf(file, sizeof(file), "%s/note.txt", other);
    assert(app_open(&a, file));
    session_load_settings(&a);
    assert(a.editor.colorscheme == COLOR_EVERFOREST && a.editor.light && a.font_size == 20);
    editor_command(&a.editor, "colorscheme tokyonight");
    app_close(&a);
    text = session_read(settings, 4096, &n);
    assert(text && strstr(text, "colorscheme = tokyonight\n"));
    free(text);
    rooms(directory);
    const char *other_files[] = {"jot.md", "AGENTS.md", "CLAUDE.md", ".tinyvi.lock", "assets", NULL};
    for (unsigned i = 0; other_files[i]; ++i) {
        char path[1300];
        snprintf(path, sizeof(path), "%s/%s", other, other_files[i]);
        remove_file(path);
        remove_folder(path);
    }
    remove_folder(other);
    remove_file(settings);
    char *folder = session_config_directory();
    remove_folder(folder);
    remove_folder(config);
    free(folder);
    free(settings);
    free(state);
    free(page);
    free(agents);
    /* Remove only the known files in this unique test directory. */
    const char *files[] = {"jot.md",        "AGENTS.md", "CLAUDE.md", ".tinyvi.lock",
                           ".tinyvi-state", "assets",    NULL};
    for (unsigned i = 0; files[i]; ++i) {
        char path[1200];
        snprintf(path, sizeof(path), "%s/%s", directory, files[i]);
#ifdef _WIN32
        wchar_t *w = windows_path(path);
        if (!DeleteFileW(w))
            RemoveDirectoryW(w);
        free(w);
#else
        if (unlink(path))
            rmdir(path);
#endif
    }
#ifdef _WIN32
    wchar_t *w = windows_path(directory);
    RemoveDirectoryW(w);
    free(w);
#else
    rmdir(directory);
#endif
    puts("Private pages, ownership, manual saves/close protection, external reload and preferences passed");
}
