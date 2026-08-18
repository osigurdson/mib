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
int main(void)
{
    char directory[1024];
#ifdef _WIN32
    char temp[MAX_PATH];
    assert(GetTempPathA(MAX_PATH, temp));
    snprintf(directory, sizeof(directory), "%stinyvi-session-%lu", temp,
             (unsigned long)GetCurrentProcessId());
#else
    snprintf(directory, sizeof(directory), "/tmp/tinyvi-session-XXXXXX");
    assert(mkdtemp(directory));
#endif
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
    /* A legacy preference disabling line numbers must no longer hide the gutter. */
    char *state = session_path(&(struct app){.directory = directory}, ".tinyvi-state");
    assert(state);
    const char *legacy = "900 600 23 0 1 1\n";
    char error[128];
    assert(platform_file_write_atomic(state, legacy, strlen(legacy), error, sizeof(error)));
    free(state);
    assert(app_open(&app, NULL));
    assert(session_open(&app, directory) == 1);
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
