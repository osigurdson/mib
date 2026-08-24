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
/* Remove the known files of a test notes folder, its pages, then the folder itself. */
static void remove_notes(const char *root)
{
    char path[1300];
    const char *top[] = {"AGENTS.md", "CLAUDE.md"};
    for (unsigned i = 0; i < 2; ++i) {
        snprintf(path, sizeof(path), "%s/%s", root, top[i]);
        remove_file(path);
    }
    for (unsigned page = 1; page <= 9; ++page) {
        const char *inside[] = {"/jot.md", "/assets", ""};
        for (unsigned i = 0; i < 3; ++i) {
            snprintf(path, sizeof(path), "%s/%u%s", root, page, inside[i]);
            remove_file(path);
            remove_folder(path);
        }
    }
    remove_folder(root);
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
static void write_file(const char *directory, const char *name, const char *text)
{
    char path[1300], error[128];
    snprintf(path, sizeof(path), "%s/%s", directory, name);
    char *slash = strrchr(path, '/');
    *slash = 0;
    assert(session_mkdir(path));
    *slash = '/';
    assert(platform_file_write_atomic(path, text, strlen(text), error, sizeof(error)));
}
static void pages(const char *parent)
{
    char directory[1100], heading[64];
    snprintf(directory, sizeof(directory), "%s/pages", parent);
    struct app app;
    assert(app_open(&app, NULL) && session_open(&app, directory) == 1);
    editor_command(&app.editor, "set view=source");
    type(&app, "ione\ntwo\x1b");
    assert(app.current == 0 && strstr(app.editor.filename, "jot.md"));
    /* Alt+digit only switches from normal mode. */
    type(&app, "a");
    app_input(&app, KEY_PAGE, "3");
    assert(app.current == 0 && app.editor.mode == INSERT);
    type(&app, "\x1b");
    size_t cursor = app.editor.cursor;
    app.editor.top = 4;
    app.follow_cursor = false;
    app_input(&app, KEY_PAGE, "3");
    assert(app.current == 2 && app.editor.length == 0 && strstr(app.editor.filename, "/3/jot.md"));
    type(&app, "i# Groceries #\nmilk\x1b");
    /* Each page keeps its own text, cursor, scroll position and undo. */
    app_input(&app, KEY_PAGE, "1");
    assert(app.current == 0 && !strcmp(app.editor.text, "one\ntwo") && app.editor.modified);
    assert(app.editor.cursor == cursor && app.editor.top == 4 && !app.follow_cursor);
    type(&app, "u");
    assert(!strcmp(app.editor.text, ""));
    app_input(&app, KEY_CTRL_R, "");
    assert(!strcmp(app.editor.text, "one\ntwo"));
    /* Quitting protects every page. */
    app_input(&app, KEY_CTRL_S, "");
    assert(!app.editor.modified);
    type(&app, ":q\n");
    assert(app.running && strstr(app.editor.message, "page 3"));
    app_request_close(&app);
    assert(app.running && strstr(app.editor.message, "page 3"));
    /* The map labels pages by their first H1, skipping fenced code. */
    write_file(directory, "4/jot.md", "intro\n```\n# not this\n```\n#nospace\n# Real\n# Later\n");
    app_page_heading(&app, 2, heading, sizeof(heading));
    assert(!strcmp(heading, "Groceries"));
    app_page_heading(&app, 3, heading, sizeof(heading));
    assert(!strcmp(heading, "Real"));
    app_page_heading(&app, 0, heading, sizeof(heading));
    assert(!heading[0]);
    app_page_heading(&app, 4, heading, sizeof(heading));
    assert(!heading[0] && !exists(directory, "5/jot.md"));
    type(&app, ":ls\n");
    assert(app.page_map);
    uint32_t pixels[640 * 480];
    assert(app_paint(&app, pixels, 640, 480, 640));
    assert(app.map_width > app.map_height && app.map_height > 0);
    assert(app_paint(&app, pixels, 120, 90, 120)); /* too small for the list: draws none */
    type(&app, "x");
    assert(app.page_map && !strcmp(app.editor.text, "one\ntwo"));
    type(&app, "3");
    assert(!app.page_map && app.current == 2 && strstr(app.editor.text, "milk"));
    type(&app, ":ls\n\x1b");
    assert(!app.page_map && app.current == 2);
    /* Alt+0 opens the list from normal mode and closes it again; :b N shows page N. */
    app_input(&app, KEY_PAGE, "0");
    assert(app.page_map && app.map_choice == 2);
    app_input(&app, KEY_PAGE, "0");
    assert(!app.page_map && app.current == 2);
    type(&app, ":b 1\n");
    assert(app.current == 0 && !strcmp(app.editor.text, "one\ntwo"));
    type(&app, ":b3\n");
    assert(app.current == 2 && strstr(app.editor.text, "milk"));
    /* Up/Down or k/j move the selection from the shown page, stopping at the ends, other
     * keys do nothing, and Enter shows the selected page. */
    type(&app, ":ls\n");
    assert(app.page_map && app.map_choice == 2);
    app_input(&app, KEY_UP, "");
    app_input(&app, KEY_UP, "");
    app_input(&app, KEY_UP, "");
    assert(app.map_choice == 0 && app.current == 2 && app.page_map);
    app_input(&app, KEY_DOWN, "");
    type(&app, "jjj");
    assert(app.map_choice == 4);
    app_input(&app, KEY_RIGHT, "");
    type(&app, "hl");
    assert(app.map_choice == 4 && app.current == 2 && strstr(app.editor.text, "milk"));
    type(&app, "jjjjjjj");
    assert(app.map_choice == 8);
    type(&app, "kkkkkkkk");
    assert(app.map_choice == 0);
    app_input(&app, KEY_ENTER, "");
    assert(!app.page_map && app.current == 0 && !strcmp(app.editor.text, "one\ntwo"));
    app_input(&app, KEY_PAGE, "3");
    /* Clicking a row shows that page; dragging on doesn't start a selection there. */
    type(&app, ":ls\n");
    assert(app_paint(&app, pixels, 640, 480, 640));
    app_pointer(&app, app.map_x + app.map_width / 2, app.map_y + 4 * app.map_height + 2, false);
    assert(!app.page_map && app.current == 4);
    app_pointer(&app, 10, 10, true);
    assert(app.editor.mode == NORMAL);
    app_pointer_release(&app);
    /* Each page has its own assets: the same name in two pages is two files, and saving a
     * page cleans up only its own. */
    char assets[1200], page_assets[1200];
    snprintf(assets, sizeof(assets), "%s/1/assets", directory);
    snprintf(page_assets, sizeof(page_assets), "%s/3/assets", directory);
    write_file(assets, "a.png", "png");
    write_file(page_assets, "a.png", "png");
    app_input(&app, KEY_PAGE, "3");
    char *image = session_path(&app, "assets/a.png");
    assert(image && strstr(image, "/3/assets/"));
    free(image);
    type(&app, "Go![](assets/a.png)\x1b:w\n");
    app_input(&app, KEY_PAGE, "1");
    image = session_path(&app, "assets/a.png");
    assert(image && !strstr(image, "/3/assets/"));
    free(image);
    type(&app, "Go![](assets/a.png)\x1b");
    assert(app_paint(&app, pixels, 640, 480, 640));
    type(&app, "dd:w\n");
    assert(!exists(assets, "a.png") && exists(page_assets, "a.png"));
    app_input(&app, KEY_PAGE, "3");
    type(&app, "dd:w\n");
    assert(!strstr(app.editor.text, "a.png") && !exists(page_assets, "a.png"));
    /* A hidden page picks up outside edits when shown again, unless it has its own. */
    app_input(&app, KEY_PAGE, "4");
    write_file(directory, "4/jot.md", "# Changed\n");
    app_input(&app, KEY_PAGE, "1");
    app_input(&app, KEY_PAGE, "4");
    assert(!strcmp(app.editor.text, "# Changed\n"));
    /* Cursor positions are remembered per page across restarts. */
    app_input(&app, KEY_PAGE, "1");
    type(&app, "gg");
    app_input(&app, KEY_PAGE, "3");
    type(&app, "ggl");
    assert(app.editor.cursor == 1);
    app_close(&app);
    /* Jot reopens on the page it closed on. */
    assert(app_open(&app, NULL) && session_open(&app, directory) == 1);
    assert(app.current == 2 && !strcmp(app.editor.text, "# Groceries #\nmilk") &&
           app.editor.cursor == 1 && app.follow_cursor);
    app_input(&app, KEY_PAGE, "1");
    assert(!strcmp(app.editor.text, "one\ntwo") && app.editor.cursor == 0);
    app_input(&app, KEY_PAGE, "3");
    assert(app.editor.cursor == 1);
    /* A page saved with :w keeps the position it was saved at, even if Jot never closes
     * cleanly; the file holds one line per page. */
    type(&app, "G:w\n");
    char *folder = session_state_directory(), positions[1200];
    snprintf(positions, sizeof(positions), "%s/positions", folder);
    free(folder);
    size_t length;
    char *saved = session_read(positions, 1 << 20, &length);
    /* Positions are keyed by the platform's absolute path. */
#ifdef _WIN32
    const char *third = "\\3\\jot.md\n", *first = "\\1\\jot.md\n";
#else
    const char *third = "/3/jot.md\n", *first = "/1/jot.md\n";
#endif
    assert(saved && !strncmp(saved, "1 0 0 ", 6) && strstr(saved, third));
    char *second_line = strchr(saved, '\n') + 1;
    assert(!strncmp(second_line, "0 0 0 ", 6) && strstr(second_line, first));
    free(saved);
    app_close(&app);
    /* Saved pages come back after a restart; unsaved, untouched pages leave no file. A page
     * with no remembered position opens at its end. */
    write_file(directory, "6/jot.md", "six\nlines\n");
    assert(app_open(&app, NULL) && session_open(&app, directory) == 1);
    app_input(&app, KEY_PAGE, "6");
    assert(app.editor.cursor == app.editor.length);
    app_input(&app, KEY_PAGE, "3");
    assert(app.editor.cursor == strlen("# Groceries #\n"));
    /* Closing with unsaved changes keeps the position from the last save; a page that was
     * only read records where it was left. */
    type(&app, "ggOnew\x1b");
    app_input(&app, KEY_PAGE, "6");
    type(&app, "gg");
    app_close(&app);
    assert(app_open(&app, NULL) && session_open(&app, directory) == 1);
    app_input(&app, KEY_PAGE, "3");
    assert(!strstr(app.editor.text, "new") && app.editor.cursor == strlen("# Groceries #\n"));
    app_input(&app, KEY_PAGE, "6");
    assert(app.editor.cursor == 0);
    assert(!exists(directory, "5/jot.md") && !exists(directory, "2/jot.md"));
    app_close(&app);
    /* :wa saves every page with changes and leaves the shown one as it was; :wqa saves and
     * quits, and :qa is :q. */
    assert(app_open(&app, NULL) && session_open(&app, directory) == 1);
    app_input(&app, KEY_PAGE, "1");
    type(&app, "ggOfirst\x1b");
    app_input(&app, KEY_PAGE, "6");
    type(&app, "ggOsixth\x1b");
    app_input(&app, KEY_PAGE, "3");
    type(&app, "ggl");
    cursor = app.editor.cursor;
    type(&app, ":qa\n");
    assert(app.running && strstr(app.editor.message, "Unsaved changes in page"));
    type(&app, ":wa\n");
    assert(app.current == 2 && app.editor.cursor == cursor && !app.editor.modified);
    assert(!app_page_modified(&app) && strstr(app.editor.message, "Saved all pages"));
    char *page_text =
        session_read(strcat(strcpy(positions, directory), "/1/jot.md"), 1 << 20, &length);
    assert(page_text && !strncmp(page_text, "first\n", 6));
    free(page_text);
    page_text = session_read(strcat(strcpy(positions, directory), "/6/jot.md"), 1 << 20, &length);
    assert(page_text && !strncmp(page_text, "sixth\n", 6));
    free(page_text);
    app_input(&app, KEY_PAGE, "1");
    assert(!strncmp(app.editor.text, "first\n", 6) && !app.editor.modified);
    type(&app, "ddu");
    app_input(&app, KEY_PAGE, "6");
    type(&app, "dd:wqa\n");
    assert(!app.running && !app_page_modified(&app) && !app.editor.modified);
    page_text = session_read(strcat(strcpy(positions, directory), "/6/jot.md"), 1 << 20, &length);
    assert(page_text && !strcmp(page_text, "six\nlines\n"));
    free(page_text);
    app_close(&app);
    /* Explicit files have no pages. */
    char file[1200];
    snprintf(file, sizeof(file), "%s/note.txt", directory);
    assert(app_open(&app, file));
    app_input(&app, KEY_PAGE, "2");
    assert(app.current == 0 && strstr(app.editor.message, "not a single file"));
    type(&app, ":ls\n");
    assert(!app.page_map && strstr(app.editor.message, "not a single file"));
    app.editor.message[0] = 0;
    app_input(&app, KEY_PAGE, "0");
    assert(!app.page_map && strstr(app.editor.message, "not a single file"));
    app.editor.message[0] = 0;
    type(&app, ":b 2\n");
    assert(app.current == 0 && strstr(app.editor.message, "not a single file"));
    app_close(&app);
    /* An explicit file opened with settings, as main does, remembers its position too. */
    write_file(directory, "note.txt", "alpha\nbeta\ngamma\n");
    assert(app_open(&app, file));
    session_load_settings(&app);
    type(&app, "jl");
    app_close(&app);
    assert(app_open(&app, file));
    session_load_settings(&app);
    assert(app.editor.cursor == strlen("alpha\nb"));
    app_close(&app);
    remove_file(file);
    remove_notes(directory);
}
static void set_env(const char *name, const char *value)
{
#ifdef _WIN32
    char setting[1300];
    snprintf(setting, sizeof(setting), "%s=%s", name, value ? value : "");
    _putenv(setting);
    SetEnvironmentVariableA(name, value);
#else
    if (value)
        setenv(name, value, 1);
    else
        unsetenv(name);
#endif
}
/* Jot opens $JOT_DIR on the page shown when it last closed, page 1 at first. */
static void last_page(const char *parent)
{
    char directory[1100];
    snprintf(directory, sizeof(directory), "%s/remembered", parent);
    set_env("JOT_DIR", directory);
    char *folder = session_state_directory(), page[1200];
    snprintf(page, sizeof(page), "%s/page", folder);
    free(folder);
    remove_file(page);
    struct app app;
    assert(app_open(&app, NULL) && session_open(&app, NULL) == 1);
    assert(app.current == 0 && strstr(app.editor.filename, "/1/jot.md"));
    app_input(&app, KEY_PAGE, "6");
    assert(app.current == 5 && strstr(app.editor.filename, "/6/jot.md"));
    app_close(&app);
    assert(app_open(&app, NULL) && session_open(&app, NULL) == 1);
    assert(app.current == 5 && strstr(app.editor.filename, "/6/jot.md"));
    /* :wqa saves other pages by swapping them in, and still remembers the shown one. */
    type(&app, "isix\x1b");
    app_input(&app, KEY_PAGE, "3");
    type(&app, "ithree\x1b");
    app_input(&app, KEY_PAGE, "2");
    type(&app, "itwo\x1b:wqa\n");
    assert(!app.running);
    app_close(&app);
    assert(app_open(&app, NULL) && session_open(&app, NULL) == 1);
    assert(app.current == 1 && strstr(app.editor.filename, "/2/jot.md"));
    app_input(&app, KEY_PAGE, "1");
    app_close(&app);
    assert(app_open(&app, NULL) && session_open(&app, NULL) == 1);
    assert(app.current == 0 && strstr(app.editor.filename, "/1/jot.md"));
    app_close(&app);
    set_env("JOT_DIR", NULL);
    assert(exists(directory, "6/jot.md") && exists(directory, "AGENTS.md"));
    remove_notes(directory);
}
int main(void)
{
    char directory[1024], config[1100], state_home[1100];
#ifdef _WIN32
    char temp[MAX_PATH];
    assert(GetTempPathA(MAX_PATH, temp));
    /* Wine reuses process IDs, so the tick count keeps runs apart. */
    snprintf(directory, sizeof(directory), "%sjot-session-%lu-%lu", temp,
             (unsigned long)GetCurrentProcessId(), (unsigned long)GetTickCount());
    snprintf(config, sizeof(config), "%s-config", directory);
    SetEnvironmentVariableA("APPDATA", config);
    char setting[1200];
    snprintf(setting, sizeof(setting), "APPDATA=%s", config);
    _putenv(setting);
    snprintf(state_home, sizeof(state_home), "%s-state", directory);
    SetEnvironmentVariableA("LOCALAPPDATA", state_home);
    snprintf(setting, sizeof(setting), "LOCALAPPDATA=%s", state_home);
    _putenv(setting);
#else
    snprintf(directory, sizeof(directory), "/tmp/jot-session-XXXXXX");
    assert(mkdtemp(directory));
    snprintf(config, sizeof(config), "%s-config", directory);
    setenv("XDG_CONFIG_HOME", config, 1);
    snprintf(state_home, sizeof(state_home), "%s-state", directory);
    setenv("XDG_STATE_HOME", state_home, 1);
#endif
    char *settings = config_file();
    struct app app;
    assert(app_open(&app, NULL));
    assert(session_open(&app, directory) == 1);
    /* A second window may open the same folder. */
    struct app second;
    assert(app_open(&second, NULL));
    assert(session_open(&second, directory) == 1);
    app_close(&second);
    /* Pages live in 1/ to 9/; the agent notes belong to the whole folder. */
    char *page = session_path(&app, "jot.md"), *agents = malloc(strlen(directory) + 16);
    assert(page && agents && strstr(page, "/1/jot.md"));
    sprintf(agents, "%s/AGENTS.md", directory);
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
    char error[128];
    assert(app_open(&app, NULL));
    assert(session_open(&app, directory) == 1);
    uint32_t pixels[320 * 200];
    assert(app_paint(&app, pixels, 320, 200, 320));
    assert(app.gutter == 6 * app.cell);
    assert(app.font_size == 23 && app.width == 900 && app.height == 600 && app.maximized);
    assert(!strcmp(app.editor.text, "another\n")); /* close did not save local edits */
    assert(!app.editor.light && app.editor.colorscheme == COLOR_JOT);
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
    const char *mode_commands[] = {"set view=source", "set view=normal", "set view=reading"};
    for (unsigned mode = VIEW_SOURCE; mode <= VIEW_READING; ++mode) {
        assert(app_open(&app, NULL));
        assert(session_open(&app, directory) == 1);
        editor_command(&app.editor, mode_commands[mode]);
        app_close(&app);
        assert(app_open(&app, NULL));
        assert(session_open(&app, directory) == 1 && app.editor.view == (enum view_mode)mode);
        assert(editor_reload(&app.editor) && app.editor.view == (enum view_mode)mode);
        app_close(&app);
    }
    for (unsigned scheme = 0; scheme < COLOR_COUNT; ++scheme) {
        for (unsigned light = 0; light < 2; ++light) {
            assert(app_open(&app, NULL));
            assert(session_open(&app, directory) == 1);
            assert(editor_colorscheme(&app.editor,
                                      editor_colorscheme_name((enum color_scheme)scheme)));
            editor_command(&app.editor, light ? "set background=light" : "set background=dark");
            app_close(&app);
            assert(app_open(&app, NULL));
            assert(session_open(&app, directory) == 1);
            assert(app.editor.colorscheme == (enum color_scheme)scheme &&
                   app.editor.light == (bool)light);
            assert(editor_reload(&app.editor) &&
                   app.editor.colorscheme == (enum color_scheme)scheme);
            app_close(&app);
        }
    }
    /* Removed palettes do not get reinterpreted as a different enum value. */
    const char *retired[] = {"default", "desert", "evening", "peachpuff"};
    for (unsigned i = 0; i < sizeof(retired) / sizeof(retired[0]); ++i) {
        char retired_settings[128];
        snprintf(retired_settings, sizeof(retired_settings),
                 "colorscheme = %s\nbackground = light\n", retired[i]);
        assert(platform_file_write_atomic(settings, retired_settings, strlen(retired_settings),
                                          error, sizeof(error)));
        assert(app_open(&app, NULL));
        assert(session_open(&app, directory) == 1);
        assert(app.editor.colorscheme == COLOR_JOT && app.editor.light);
        app_close(&app);
    }

    /* Every page shares one set of settings. render = polish is the old spelling of view = reading.
     */
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
               p->editor.view == VIEW_READING && p->font_size == 20 && p->width == 1000 &&
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
           a.editor.view == VIEW_READING);
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
    pages(directory);
    last_page(directory);
    remove_notes(other);
    remove_file(settings);
    char *folder = session_state_directory(), positions[1200];
    snprintf(positions, sizeof(positions), "%s/positions", folder);
    remove_file(positions);
    snprintf(positions, sizeof(positions), "%s/page", folder);
    remove_file(positions);
    remove_folder(folder);
    remove_folder(state_home);
    free(folder);
    folder = session_config_directory();
    remove_folder(folder);
    remove_folder(config);
    free(folder);
    free(settings);
    free(page);
    free(agents);
    /* Remove only the known files in this unique test directory. */
    remove_notes(directory);
    puts("Private pages, shared folders, manual saves/close protection, external reload and "
         "preferences passed");
}
