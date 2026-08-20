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
