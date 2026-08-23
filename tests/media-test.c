#define _POSIX_C_SOURCE 200809L
#include "app.h"
#include "media.h"
#include "platform.h"
#include "session.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include "platform/windows/win32.h"
#else
#include <unistd.h>
#endif
static bool exists(const char *path)
{
    bool missing;
    char error[128];
    FILE *f = platform_file_open_read(path, &missing, error, sizeof(error));
    if (f)
        fclose(f);
    return f != NULL;
}
static void remove_dir(const char *path)
{
#ifdef _WIN32
    wchar_t *w = windows_path(path);
    assert(RemoveDirectoryW(w));
    free(w);
#else
    assert(!rmdir(path));
#endif
}
/* Each page keeps images in its own folder; yanking an image in one page and pasting it in
 * another copies the file into the second page's assets. */
static void pages_test(const char *root, const unsigned char *png, size_t bytes)
{
    char directory[1200];
    snprintf(directory, sizeof(directory), "%s/pages", root);
    struct app app;
    assert(app_open(&app, NULL) && session_open(&app, directory) == 1);
    assert(media_paste_image(&app, png, bytes));
    char *first = media_target(&app);
    assert(first && strstr(first, "/1/assets/"));
    app.editor.mode = NORMAL;
    app.editor.cursor = 0;
    app_input(&app, KEY_TEXT, "y");
    app_input(&app, KEY_TEXT, "y");
    size_t length;
    char *fragment = media_fragment(&app, &length);
    assert(fragment);
    app_input(&app, KEY_CTRL_S, "");
    assert(app_page_switch(&app, 1));
    assert(media_paste_fragment(&app, (unsigned char *)fragment, length));
    free(fragment);
    app.editor.cursor = (size_t)(strstr(app.editor.text, "![](") - app.editor.text);
    const struct media_entry *copied = media_at(&app, app.editor.cursor);
    assert(copied && copied->width == 320);
    char *second = media_target(&app);
    assert(second && strstr(second, "/2/assets/") && exists(second) && exists(first));
    app_input(&app, KEY_CTRL_S, "");
    /* Removing the copy from page 2 leaves page 1's image alone. */
    app_input(&app, KEY_TEXT, "d");
    app_input(&app, KEY_TEXT, "d");
    app_input(&app, KEY_CTRL_S, "");
    assert(!exists(second) && exists(first));
    app_close(&app);
    char path[1300];
    const char *files[] = {"2/jot.md", "1/jot.md", "AGENTS.md", "CLAUDE.md"};
    for (unsigned i = 0; i < 4; ++i) {
        snprintf(path, sizeof(path), "%s/%s", directory, files[i]);
        assert(platform_file_remove(path));
    }
    assert(platform_file_remove(first));
    const char *folders[] = {"2/assets", "2", "1/assets", "1", ""};
    for (unsigned i = 0; i < 5; ++i) {
        snprintf(path, sizeof(path), "%s/%s", directory, folders[i]);
        remove_dir(path);
    }
    free(first);
    free(second);
}
/* Closing a window removes the images it pasted that no saved page mentions, on every page,
 * with their OCR text, as when a paste is undone or the window closes with :q!. */
static void close_test(const char *root, const unsigned char *png, size_t bytes)
{
    char directory[1200];
    snprintf(directory, sizeof(directory), "%s/close", root);
    struct app app;
    assert(app_open(&app, NULL) && session_open(&app, directory) == 1);
    assert(app_page_switch(&app, 0));            /* the previous test closed on page 2 */
    assert(media_paste_image(&app, png, bytes)); /* page 1, never saved */
    char *unsaved = media_target(&app), sidecar[1400];
    assert(unsaved);
    snprintf(sidecar, sizeof(sidecar), "%s.txt", unsaved);
    assert(session_write_new(sidecar, "ocr", 3));
    assert(app_page_switch(&app, 1));
    assert(media_paste_image(&app, png, bytes)); /* page 2, saved */
    char *saved = media_target(&app);
    app_input(&app, KEY_CTRL_S, "");
    assert(media_paste_image(&app, png, bytes)); /* page 2, pasted then undone */
    app.editor.cursor = app.editor.length - 1;
    char *undone = media_target(&app);
    assert(saved && undone && strcmp(saved, undone));
    app_input(&app, KEY_ESCAPE, "");
    app_input(&app, KEY_TEXT, "u");
    assert(!app.editor.modified);
    /* A second window that never pasted anything removes nothing. */
    struct app other;
    assert(app_open(&other, NULL) && session_open(&other, directory) == 1);
    app_close(&other);
    assert(exists(unsaved) && exists(sidecar) && exists(undone));
    app_close(&app);
    assert(!exists(unsaved) && !exists(sidecar) && !exists(undone) && exists(saved));
    char path[1300];
    const char *files[] = {"1/jot.md", "2/jot.md", "AGENTS.md", "CLAUDE.md"};
    for (unsigned i = 0; i < 4; ++i) {
        snprintf(path, sizeof(path), "%s/%s", directory, files[i]);
        assert(platform_file_remove(path));
    }
    assert(platform_file_remove(saved));
    const char *folders[] = {"2/assets", "2", "1/assets", "1", ""};
    for (unsigned i = 0; i < 5; ++i) {
        snprintf(path, sizeof(path), "%s/%s", directory, folders[i]);
        remove_dir(path);
    }
    free(unsaved);
    free(saved);
    free(undone);
}
static void cleanup_test(const char *root, const unsigned char *png, size_t bytes)
{
    char directory[1200];
    snprintf(directory, sizeof(directory), "%s/cleanup", root);
    struct app app;
    assert(app_open(&app, NULL));
    assert(session_open(&app, directory) == 1);
    assert(media_paste_image(&app, png, bytes));
    media_sync(&app);
    const struct media_entry *e = media_at(&app, 0);
    assert(e);
    char name[256];
    snprintf(name, sizeof(name), "%s", e->name);
    char *path = session_path(&app, name);
    char side[272];
    snprintf(side, sizeof(side), "%s.txt", name);
    char *txt = session_path(&app, side);
    snprintf(side, sizeof(side), "%s.tsv", name);
    char *tsv = session_path(&app, side);
    assert(session_write_new(txt, "text", 4));
    assert(session_write_new(tsv, "tsv", 3));
    app_input(&app, KEY_CTRL_S, "");
    assert(exists(path) && app.editor.history); /* Ordinary save keeps undo. */
    /* Unsaved deletion must not damage the saved note, even across closing. */
    app.editor.cursor = 0;
    app.editor.mode = NORMAL;
    app_input(&app, KEY_DELETE, "");
    assert(exists(path) && exists(txt) && exists(tsv));
    editor_undo(&app.editor, false);
    assert(strstr(app.editor.text, name));
    app.editor.cursor = 0;
    app_input(&app, KEY_DELETE, "");
    app_close(&app);
    assert(exists(path));
    assert(app_open(&app, NULL));
    assert(session_open(&app, directory) == 1);
    assert(strstr(app.editor.text, name));
    /* A remaining inline reference retains the asset, even in raw mode. */
    char reference[300];
    snprintf(reference, sizeof(reference), "inline ![](%s) text\n", name);
    assert(editor_replace(&app.editor, 0, app.editor.length, reference, strlen(reference)));
    app.editor.view = VIEW_SOURCE;
    app_input(&app, KEY_CTRL_S, "");
    assert(exists(path) && app.editor.history);
    assert(editor_replace(&app.editor, 0, app.editor.length, "", 0));
    /* Failed saves retain the asset and history. */
    char *filename = app.editor.filename;
    app.editor.filename = directory;
    app_input(&app, KEY_CTRL_S, "");
    assert(app.editor.modified && exists(path) && exists(txt) && app.editor.history);
    app.editor.filename = filename;
    /* Untracked files are not swept from assets. */
    char *orphan = session_path(&app, "assets/untracked.png");
    assert(session_write_new(orphan, "unused", 6));
    /* A directory in place of the image makes unlink fail deterministically. */
    assert(platform_file_remove(path));
    assert(session_mkdir(path));
    app_input(&app, KEY_CTRL_S, "");
    assert(!app.editor.modified && exists(txt) && app.editor.history);
    assert(strstr(app.editor.message, "could not be removed"));
#ifdef _WIN32
    wchar_t *blocked = windows_path(path);
    assert(RemoveDirectoryW(blocked));
    free(blocked);
#else
    assert(!rmdir(path));
#endif
    assert(session_write_new(path, (const char *)png, bytes));
    app_input(&app, KEY_CTRL_S, "");
    assert(!exists(path) && !exists(txt) && !exists(tsv));
    assert(exists(orphan) && !app.editor.modified && !app.editor.history);
    app_input(&app, KEY_TEXT, "u");
    assert(!strstr(app.editor.text, name));
    /* Saving an image deletion in insert mode starts a fresh grouped edit. */
    app.editor.view = VIEW_NORMAL;
    assert(media_paste_image(&app, png, bytes));
    app.editor.mode = INSERT;
    app.editor.cursor = 0;
    app_input(&app, KEY_DELETE, "");
    app_input(&app, KEY_CTRL_S, "");
    app_input(&app, KEY_TEXT, "a");
    app_input(&app, KEY_TEXT, "b");
    app_input(&app, KEY_ESCAPE, "");
    app_input(&app, KEY_TEXT, "u");
    assert(!app.editor.length);
    /* Imported images removed before saving are cleaned by :wq as well. */
    assert(media_paste_image(&app, png, bytes));
    e = media_at(&app, 0);
    assert(e);
    char *imported = session_path(&app, e->name);
    assert(editor_replace(&app.editor, 0, app.editor.length, "", 0));
    app_input(&app, KEY_TEXT, ":");
    app_input(&app, KEY_TEXT, "wq");
    app_input(&app, KEY_ENTER, "");
    assert(!app.running && !exists(imported));
    free(imported);
    app_close(&app);
    assert(platform_file_remove(orphan));
    free(orphan);
    free(path);
    free(txt);
    free(tsv);
    /* Leave no temporary page files behind. */
    const char *files[] = {"1/jot.md", "AGENTS.md", "CLAUDE.md"};
    for (unsigned i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
        char full[1400];
        snprintf(full, sizeof(full), "%s/%s", directory, files[i]);
        assert(platform_file_remove(full));
    }
    const char *folders[] = {"1/assets", "1", ""};
    for (unsigned i = 0; i < 3; ++i) {
        char full[1400];
        snprintf(full, sizeof(full), "%s/%s", directory, folders[i]);
        remove_dir(full);
    }
}
static void image_typing_test(const char *directory, const unsigned char *png, size_t bytes)
{
    struct app app;
    assert(app_open(&app, NULL));
    app.directory = strdup(directory);
    assert(app.directory);
    app_input(&app, KEY_TEXT, "i");
    assert(media_paste_image(&app, png, bytes));
    const char *reference = strstr(app.editor.text, "![](");
    assert(reference);
    char *markup = strdup(reference);
    assert(markup);
    const struct media_entry *image = media_at(&app, 0);
    assert(image);
    char *path = session_path(&app, image->name);
    assert(path);
    app_input(&app, KEY_TEXT, "after paste");
    reference = strstr(app.editor.text, "![](");
    assert(reference && media_at(&app, (size_t)(reference - app.editor.text)));
    assert(!strncmp(app.editor.text, markup, strlen(markup)));
    assert(strstr(app.editor.text, "\nafter paste"));
    uint32_t *pixels = malloc(320 * 240 * sizeof(*pixels));
    assert(pixels && app_paint(&app, pixels, 320, 240, 320));
    assert(app_view_rows(&app, 0) > 1);
    free(pixels);
    app_input(&app, KEY_ESCAPE, "");
    const char *keys[] = {"i", "I", "a", "A"};
    for (unsigned i = 0; i < 4; ++i) {
        assert(editor_replace(&app.editor, 0, app.editor.length, markup, strlen(markup)));
        app.editor.cursor = strlen(markup) / 2; /* hidden source offset on the image */
        app.editor.mode = NORMAL;
        app_input(&app, KEY_TEXT, keys[i]);
        app_input(&app, KEY_TEXT, "near image");
        app_input(&app, KEY_ESCAPE, "");
        reference = strstr(app.editor.text, "![](");
        assert(reference && media_at(&app, (size_t)(reference - app.editor.text)));
        assert(strstr(app.editor.text, markup));
        assert(i < 2 ? !strncmp(app.editor.text, "near image\n", 11)
                     : strstr(app.editor.text, "\nnear image\n") != NULL);
        app_input(&app, KEY_TEXT, "u");
        assert(!strcmp(app.editor.text, markup));
        app_input(&app, KEY_CTRL_R, "");
        reference = strstr(app.editor.text, "![](");
        assert(reference && media_at(&app, (size_t)(reference - app.editor.text)));
    }
    /* Insert-mode navigation onto an image must not turn literal d into delete. */
    assert(editor_replace(&app.editor, 0, app.editor.length, markup, strlen(markup)));
    assert(editor_replace(&app.editor, 0, 0, "\n", 1));
    app.editor.cursor = 0;
    app.editor.mode = INSERT;
    app_input(&app, KEY_DOWN, "");
    assert(media_at(&app, app.editor.cursor));
    app_input(&app, KEY_TEXT, "d");
    assert(app.editor.mode == INSERT && !strncmp(app.editor.text, "\nd\n", 3));
    reference = strstr(app.editor.text, "![](");
    assert(reference && media_at(&app, (size_t)(reference - app.editor.text)) && exists(path));
    app_input(&app, KEY_ESCAPE, "");
    app_input(&app, KEY_TEXT, "u");
    assert(app.editor.length == strlen(markup) + 1 && !strcmp(app.editor.text + 1, markup));
    for (unsigned action = 0; action < 5; ++action) {
        assert(editor_replace(&app.editor, 0, app.editor.length, markup, strlen(markup)));
        app.editor.cursor = strlen(markup) / 2;
        app.editor.mode = INSERT;
        if (action == 0)
            app_input(&app, KEY_TEXT, "typed");
        else if (action == 1)
            app_input(&app, KEY_TAB, "");
        else if (action == 2) {
            app_input(&app, KEY_ENTER, "");
            app_input(&app, KEY_TEXT, "paragraph");
        } else {
            app.editor.mode = action == 3 ? INSERT : NORMAL;
            app.editor.put_after = true;
            media_paste_text(&app, "clipboard", 9);
        }
        app_input(&app, KEY_ESCAPE, "");
        reference = strstr(app.editor.text, "![](");
        assert(reference && media_at(&app, (size_t)(reference - app.editor.text)));
        assert(strstr(app.editor.text, markup));
        app_input(&app, KEY_TEXT, "u");
        assert(!strcmp(app.editor.text, markup));
    }
    /* Editing across image boundaries must remove a block, never half its source. */
    char surrounded[512];
    snprintf(surrounded, sizeof(surrounded), "before\n%safter", markup);
    const enum editor_key deletes[] = {KEY_BACKSPACE, KEY_DELETE, KEY_BACKSPACE,
                                       KEY_CTRL_W,    KEY_CTRL_U, KEY_TEXT};
    for (unsigned i = 0; i < sizeof(deletes) / sizeof(deletes[0]); ++i) {
        assert(editor_replace(&app.editor, 0, app.editor.length, surrounded, strlen(surrounded)));
        app.editor.mode = i == 5 ? NORMAL : INSERT;
        app.editor.cursor = i == 0 ? 7 + strlen(markup) : i == 1 ? 6 : 10;
        app_input(&app, deletes[i], i == 5 ? "x" : "");
        assert(!strcmp(app.editor.text, "before\nafter") && exists(path));
        app_input(&app, KEY_ESCAPE, "");
        app_input(&app, KEY_TEXT, "u");
        assert(!strcmp(app.editor.text, surrounded));
    }
    /* Pasting another image while the caret is inside hidden source is atomic. */
    assert(editor_replace(&app.editor, 0, app.editor.length, markup, strlen(markup)));
    app.editor.mode = INSERT;
    app.editor.cursor = 2;
    assert(media_paste_image(&app, png, bytes));
    media_sync(&app);
    const struct media_entry *first = media_at(&app, 0);
    assert(first);
    size_t next_image = first->end + 1;
    assert(media_at(&app, next_image) && !strcmp(app.editor.text + next_image, markup));
    app_input(&app, KEY_ESCAPE, "");
    assert(editor_replace(&app.editor, 0, app.editor.length, markup, strlen(markup)));
    /* Raw mode continues to edit the literal source, without image redirects. */
    app.editor.view = VIEW_SOURCE;
    app.editor.cursor = 0;
    app_input(&app, KEY_TEXT, "i");
    app_input(&app, KEY_TEXT, "literal");
    app_input(&app, KEY_ESCAPE, "");
    assert(!strncmp(app.editor.text, "literal![](", 11));
    app_input(&app, KEY_TEXT, "u");
    app.editor.view = VIEW_NORMAL;
    /* Saving and reopening keeps the image reference and asset intact. */
    char *note_path = session_path(&app, "typing.md");
    assert(note_path && editor_save(&app.editor, note_path));
    app_close(&app);
    assert(app_open(&app, note_path));
    assert(media_at(&app, 0));
    assert(!strcmp(app.editor.text, markup));
    app_close(&app);
    assert(platform_file_remove(path));
    free(path);
    free(markup);
    assert(platform_file_remove(note_path));
    free(note_path);
}
/* The alt text of the first image line, such as "|352" or "shot". */
static const char *alt(const struct app *app)
{
    static char out[64];
    const char *open = strstr(app->editor.text, "![");
    const char *close = open ? strstr(open, "](") : NULL;
    assert(open && close && close - open - 2 < (long)sizeof(out));
    memcpy(out, open + 2, (size_t)(close - open - 2));
    out[close - open - 2] = 0;
    return out;
}
static void keys(struct app *app, const char *typed)
{
    for (const char *p = typed; *p; ++p) {
        char key[2] = {*p, 0};
        app_input(app, KEY_TEXT, key);
    }
}
static void command(struct app *app, const char *line)
{
    app_input(app, KEY_TEXT, ":");
    keys(app, line);
    app_input(app, KEY_ENTER, "");
}
static void resize_test(const char *root, const unsigned char *png, size_t bytes)
{
    /* Open a real page: session_open replaces the editor, as the app does at startup. */
    char directory[1200];
    snprintf(directory, sizeof(directory), "%s/resize", root);
    struct app app;
    assert(app_open(&app, NULL));
    assert(session_open(&app, directory) == 1);
    assert(media_paste_image(&app, png, bytes));
    app_input(&app, KEY_ESCAPE, "");
    app.editor.cursor = 0;
    static uint32_t pixels[800 * 560];
    assert(app_paint(&app, pixels, 800, 560, 800));
    assert(app.text_width > 400 && app.image_box && app.image_box_w == 320);
    assert(!strcmp(alt(&app), ""));

    /* + and - change the width by 10% of what is shown, with counts; = resets. */
    keys(&app, "+");
    assert(!strcmp(alt(&app), "|352") && media_at(&app, app.editor.cursor));
    keys(&app, "2-");
    assert(!strcmp(alt(&app), "|282"));
    keys(&app, "=");
    assert(!strcmp(alt(&app), ""));
    keys(&app, "-");
    assert(!strcmp(alt(&app), "|288"));
    keys(&app, ".");
    assert(!strcmp(alt(&app), "|259"));
    keys(&app, "u");
    assert(!strcmp(alt(&app), "|288"));
    keys(&app, "u");
    assert(!strcmp(alt(&app), ""));

    /* :resize sets exact or relative widths and clamps them to the text area. */
    command(&app, "resize 200");
    assert(!strcmp(alt(&app), "|200"));
    command(&app, "res 50%");
    assert(!strcmp(alt(&app), "|160"));
    command(&app, "resize +10");
    assert(!strcmp(alt(&app), "|170"));
    command(&app, "resize -10%");
    assert(!strcmp(alt(&app), "|153"));
    command(&app, "resize 99999");
    char widest[16];
    snprintf(widest, sizeof(widest), "|%d", app.text_width);
    assert(!strcmp(alt(&app), widest));
    command(&app, "resize 1");
    assert(!strcmp(alt(&app), "|16"));
    command(&app, "resize wide");
    assert(!strcmp(alt(&app), "|16") && strstr(app.editor.message, "Use :resize"));
    command(&app, "resize");
    assert(!strcmp(alt(&app), ""));

    /* The stored width is logical pixels: it doubles on a 2x display. */
    command(&app, "resize 200");
    int w, h;
    media_display_size(&app, media_at(&app, 0), 0, &w, &h);
    assert(w == 200 && h == 112);
    app.scale120 = 240;
    media_display_size(&app, media_at(&app, 0), 0, &w, &h);
    assert(w == 400 && h == 225);
    media_display_size(&app, media_at(&app, 0), 300, &w, &h);
    assert(w == 300 && h == 168);
    app.scale120 = 120;
    assert(app_paint(&app, pixels, 800, 560, 800));
    assert(app.image_box_w == 200 && app.image_box_h == 112);

    /* Obsidian's other forms are read, and existing alt text is kept. */
    const struct media_entry *e = media_at(&app, 0);
    assert(editor_replace(&app.editor, e->start + 2, e->alt_end, "shot|100x80", 11));
    app.editor.cursor = 0;
    assert(media_at(&app, 0)->size == 100);
    keys(&app, "+");
    assert(!strcmp(alt(&app), "shot|110"));
    keys(&app, "=");
    assert(!strcmp(alt(&app), "shot"));
    e = media_at(&app, 0);
    assert(editor_replace(&app.editor, e->start + 2, e->alt_end, "250", 3));
    app.editor.cursor = 0;
    assert(media_at(&app, 0)->size == 250);
    keys(&app, "-");
    assert(!strcmp(alt(&app), "|225"));
    e = media_at(&app, 0);
    assert(editor_replace(&app.editor, e->start + 2, e->alt_end, "a|b", 3));
    app.editor.cursor = 0;
    assert(media_at(&app, 0)->size == 0);
    keys(&app, "+");
    assert(!strcmp(alt(&app), "a|b|352"));
    e = media_at(&app, 0);
    assert(editor_replace(&app.editor, e->start + 2, e->alt_end, "", 0));
    app.editor.cursor = 0;

    /* Reading is read-only, source shows source, and text lines keep their behaviour. */
    editor_command(&app.editor, "set view=reading");
    keys(&app, "+");
    assert(!strcmp(alt(&app), ""));
    editor_command(&app.editor, "set view=source");
    keys(&app, "+");
    assert(!strcmp(alt(&app), ""));
    editor_command(&app.editor, "set view=normal");
    size_t text_line = app.editor.length;
    assert(editor_replace(&app.editor, text_line, text_line, "text\n", 5));
    app.editor.cursor = text_line;
    uint64_t revision = app.editor.revision;
    keys(&app, "+");
    assert(app.editor.revision == revision);
    command(&app, "resize 100");
    assert(app.editor.revision == revision && strstr(app.editor.message, "No image"));

    /* Dragging a corner handle previews the width and commits one undo step. */
    app.editor.cursor = 0;
    assert(app_paint(&app, pixels, 800, 560, 800));
    int right = app.image_box_x + app.image_box_w, bottom = app.image_box_y + app.image_box_h;
    assert(app_image_corner(&app, right, bottom) == 3);
    assert(app_image_corner(&app, app.image_box_x, app.image_box_y) == 0);
    assert(app_image_corner(&app, right + 40, bottom + 40) == -1);
    revision = app.editor.revision;
    app_pointer(&app, right, bottom, false);
    assert(app.image_drag && app.editor.cursor == 0);
    app_pointer(&app, right - 80, bottom, true);
    assert(app.drag_width == 240 && app.editor.revision == revision);
    assert(app_paint(&app, pixels, 800, 560, 800) && app.image_box_w == 240);
    /* A vertical drag scales by the aspect ratio. */
    app_pointer(&app, right, bottom + 18, true);
    assert(app.drag_width == 352);
    app_pointer_release(&app);
    assert(!app.image_drag && !strcmp(alt(&app), "|352"));
    keys(&app, "u");
    assert(!strcmp(alt(&app), ""));
    /* A click that does not move leaves the text alone. */
    assert(app_paint(&app, pixels, 800, 560, 800));
    revision = app.editor.revision;
    app_pointer(&app, app.image_box_x, app.image_box_y, false);
    assert(app.image_drag);
    app_pointer_release(&app);
    assert(app.editor.revision == revision);
    /* Handles only work on the selected image. */
    app.editor.cursor = text_line;
    assert(app_paint(&app, pixels, 800, 560, 800) && !app.image_box);
    app_close(&app);
}
int main(int argc, char **argv)
{
    char directory[1024];
#ifdef _WIN32
    SetEnvironmentVariableA("JOT_OCR", "0");
    _putenv("JOT_OCR=0");
    char temp[MAX_PATH];
    assert(GetTempPathA(MAX_PATH, temp));
    snprintf(directory, sizeof(directory), "%sjot-media-%lu-%lu", temp,
             (unsigned long)GetCurrentProcessId(), (unsigned long)GetTickCount());
    /* Keep settings, positions and the remembered page out of the Wine profile, as make
     * test does with XDG_CONFIG_HOME and XDG_STATE_HOME on Linux. */
    char setting[1200];
    snprintf(setting, sizeof(setting), "APPDATA=%s-config", directory);
    _putenv(setting);
    SetEnvironmentVariableA("APPDATA", setting + strlen("APPDATA="));
    snprintf(setting, sizeof(setting), "LOCALAPPDATA=%s-state", directory);
    _putenv(setting);
    SetEnvironmentVariableA("LOCALAPPDATA", setting + strlen("LOCALAPPDATA="));
#else
    setenv("JOT_OCR", "0", 1);
    snprintf(directory, sizeof(directory), "/tmp/jot-media-XXXXXX");
    assert(mkdtemp(directory));
#endif
    unsigned char rgba[320 * 180 * 4];
    for (unsigned y = 0; y < 180; ++y)
        for (unsigned x = 0; x < 320; ++x) {
            size_t p = (y * 320 + x) * 4;
            rgba[p] = (unsigned char)(x * 255 / 320);
            rgba[p + 1] = (unsigned char)(y * 255 / 180);
            rgba[p + 2] = 100;
            rgba[p + 3] = 255;
        }
    size_t bytes;
    unsigned char *png = image_png(rgba, 320, 180, &bytes);
    assert(png);
    image_typing_test(directory, png, bytes);
    resize_test(directory, png, bytes);
    cleanup_test(directory, png, bytes);
    pages_test(directory, png, bytes);
    close_test(directory, png, bytes);
    int w, h;
    assert(image_info(png, bytes, &w, &h) && w == 320 && h == 180);
    struct image image = {0};
    assert(image_decode(&image, png, bytes));
    assert(!memcmp(image.rgba, rgba, sizeof(rgba)));
    image_free(&image);
    assert(!image_decode(&image, png, 12));
    struct app app;
    assert(app_open(&app, NULL));
    assert(session_open(&app, directory) == 1);
    assert(!app.media);
    assert(media_paste_image(&app, png, bytes));
    uint64_t original_revision = app.editor.revision;
    size_t original_length = app.editor.length;
    editor_command(&app.editor, "set view=reading");
    assert(!media_paste_image(&app, png, bytes));
    media_paste_text(&app, "cannot paste", 12);
    media_import_file(&app, "unused.png");
    media_import_uris(&app, "file:///unused.png", 18);
    assert(app.editor.revision == original_revision && app.editor.length == original_length);
    editor_command(&app.editor, "set view=source");
    uint32_t raw_pixels[320 * 200];
    assert(app_paint(&app, raw_pixels, 320, 200, 320));
    assert(app_view_rows(&app, 0) == 1 && app_view_next(&app, 0) < app.editor.length);
    app_input(&app, KEY_TEXT, "d");
    assert(app.editor.revision == original_revision && app.editor.pending[0] == 'd');
    app_input(&app, KEY_ESCAPE, "");
    editor_command(&app.editor, "set view=normal");
    free(png);
    assert(strstr(app.editor.text, "![](assets/"));
    const struct media_entry *entry = media_at(&app, app.editor.cursor);
    assert(entry && entry->width == 320 && entry->height == 180);
    char relative[272], name[256];
    snprintf(name, sizeof(name), "%s", entry->name);
    snprintf(relative, sizeof(relative), "%s.txt", name);
    char *path = session_path(&app, relative);
    assert(session_write_new(path, "Hello screenshot OCR\n", 21));
    free(path);
    snprintf(relative, sizeof(relative), "%s.tsv", name);
    path = session_path(&app, relative);
    const char *tsv = "level\tpage_num\tblock_num\tpar_num\tline_num\tword_"
                      "num\tleft\ttop\twidth\theight\tconf\ttext\n5\t1\t1\t1\t1\t1\t10\t10\t100\t30"
                      "\t90\tHello\n5\t1\t1\t1\t1\t2\t120\t10\t100\t30\t90\tscreenshot\n";
    assert(session_write_new(path, tsv, strlen(tsv)));
    free(path);
    app_input(&app, KEY_TEXT, "G");
    app_input(&app, KEY_TEXT, "/");
    app_input(&app, KEY_TEXT, "Hello screenshot");
    app_input(&app, KEY_ENTER, "");
    assert(media_at(&app, app.editor.cursor));
    assert(app.image_hit);
    const uint32_t scales[] = {120, 150, 180, 240};
    const unsigned sizes[] = {12, 16, 18, 22};
    for (unsigned i = 0; i < 4; ++i) {
        app.editor.light = i % 2 != 0;
        app.scale120 = scales[i];
        app.font_size = sizes[i];
        app.editor.top = 0;
        uint32_t *pixels = malloc(800 * 560 * 4);
        assert(pixels);
        assert(app_paint(&app, pixels, 800, 560, 800));
        assert(app_view_rows(&app, 0) > 1);
        int image_x = app.padding + app.gutter, image_y = app.padding + 2;
        assert(pixels[(size_t)(image_y - 3) * 800 + image_x - 3] ==
               (app.editor.light ? 0xff263342 : 0xffe5edf5));
        assert(pixels[(size_t)(image_y - 3) * 800 + image_x + 10] ==
               (app.editor.light ? 0xff176778 : 0xff88c0d0));
        int px = app.padding + app.gutter + 115, py = app.padding + 2 + 20;
        uint32_t raw = 0xff000000u | ((115u * 255 / 320) << 16) | ((20u * 255 / 180) << 8) | 100;
        int sample_x = image_x + 30, sample_y = image_y + 70;
        uint32_t original = pixels[(size_t)sample_y * 800 + sample_x];
        app.editor.light = !app.editor.light;
        assert(app_paint(&app, pixels, 800, 560, 800));
        assert(pixels[(size_t)sample_y * 800 + sample_x] == original);
        app.editor.light = !app.editor.light;
        assert(app_paint(&app, pixels, 800, 560, 800));
        assert(pixels[(size_t)py * 800 + px] != raw); /* phrase spans the gap between words */
        uint32_t highlighted = pixels[(size_t)py * 800 + px];
        app.image_hit = false;
        assert(app_paint(&app, pixels, 800, 560, 800));
        uint32_t unhighlighted = pixels[(size_t)py * 800 + px];
        assert(unhighlighted != highlighted);
        app.image_hit = true;
        app.editor.search_hidden = true;
        assert(app_paint(&app, pixels, 800, 560, 800));
        assert(pixels[(size_t)py * 800 + px] == unhighlighted);
        app.editor.search_hidden = false;
        app.editor.mode = INSERT;
        assert(app_paint(&app, pixels, 800, 560, 800));
        assert(pixels[(size_t)py * 800 + px] == unhighlighted);
        app.editor.mode = NORMAL;
        assert(app_paint(&app, pixels, 800, 560, 800));
        assert(app_hit_test(&app, 100 * 120 / scales[i], 60 * 120 / scales[i]) == 0);
        if (argc > 1) {
            char output[1024];
            snprintf(output, sizeof(output), "%s-%u.ppm", argv[1], i);
            FILE *f = fopen(output, "wb");
            assert(f);
            fprintf(f, "P6\n800 560\n255\n");
            for (int p = 0; p < 800 * 560; ++p) {
                unsigned char rgb[] = {pixels[p] >> 16, pixels[p] >> 8, pixels[p]};
                fwrite(rgb, 1, 3, f);
            }
            fclose(f);
        }
        free(pixels);
    }
    char *target = media_target(&app);
    assert(target && strstr(target, name));
    free(target);
    size_t n;
    char *ocr = media_ocr_text(&app, &n);
    assert(ocr && strstr(ocr, "screenshot"));
    free(ocr);
    /* Normal-mode d and Delete in either mode remove the entire image. */
    for (unsigned mode = 0; mode < 2; ++mode)
        for (unsigned del = 0; del < 2; ++del) {
            if (mode && !del)
                continue;
            app.editor.cursor = 0;
            app.editor.mode = mode ? INSERT : NORMAL;
            enum editor_mode expected_mode = app.editor.mode;
            app_input(&app, del ? KEY_DELETE : KEY_TEXT, del ? "" : "d");
            assert(!strstr(app.editor.text, "![]"));
            assert(app.editor.mode == expected_mode);
            assert(app.editor.register_linewise && strstr(app.editor.register_text, "![]"));
            editor_undo(&app.editor, false);
            assert(strstr(app.editor.text, "![]"));
        }
    app.editor.mode = NORMAL;
    /* A command's d remains command text even while an image is selected. */
    app.editor.cursor = 0;
    app_input(&app, KEY_TEXT, ":");
    app_input(&app, KEY_TEXT, "d");
    assert(strstr(app.editor.text, "![]") && !strcmp(app.editor.command, "d"));
    app_input(&app, KEY_ESCAPE, "");
    /* Mixed notes retain their images when copied into a different page. */
    assert(editor_replace(&app.editor, 0, 0, "before\n", 7));
    assert(editor_replace(&app.editor, app.editor.length, app.editor.length, "after\n", 6));
    /* Deleting an image between notes leaves both neighboring lines intact. */
    char *image_ref = strstr(app.editor.text, "![](");
    assert(image_ref);
    app.editor.cursor = (size_t)(image_ref - app.editor.text);
    app_input(&app, KEY_DELETE, "");
    assert(!strcmp(app.editor.text, "before\nafter\n"));
    app_input(&app, KEY_TEXT, "u");
    assert(strstr(app.editor.text, "![]("));
    app.editor.cursor = 0;
    app_input(&app, KEY_TEXT, "V");
    app_input(&app, KEY_TEXT, "G");
    app_input(&app, KEY_TEXT, "y");
    size_t fragment_length;
    char *fragment = media_fragment(&app, &fragment_length);
    assert(fragment);
    struct app other;
    assert(app_open(&other, NULL));
    other.directory = session_path(&app, "other");
    assert(other.directory && session_mkdir(other.directory));
    editor_command(&other.editor, "set view=reading");
    assert(!media_paste_fragment(&other, (unsigned char *)fragment, fragment_length));
    assert(other.editor.length == 0);
    editor_command(&other.editor, "set view=normal");
    assert(media_paste_fragment(&other, (unsigned char *)fragment, fragment_length));
    free(fragment);
    assert(strstr(other.editor.text, "before\n") && strstr(other.editor.text, "after\n"));
    char *ref = strstr(other.editor.text, "![](");
    assert(ref);
    other.editor.cursor = (size_t)(ref - other.editor.text);
    const struct media_entry *copied = media_at(&other, other.editor.cursor);
    assert(copied && copied->width == 320 && copied->height == 180);
    char *copied_path = media_target(&other), *other_assets = session_path(&other, "assets");
    assert(copied_path && other_assets);
    char *other_directory = malloc(strlen(other.directory) + 1);
    assert(other_directory);
    strcpy(other_directory, other.directory);
    app_close(&other);
#ifdef _WIN32
    wchar_t *wc = windows_path(copied_path), *wa2 = windows_path(other_assets),
            *wd2 = windows_path(other_directory);
    assert(DeleteFileW(wc));
    assert(RemoveDirectoryW(wa2));
    assert(RemoveDirectoryW(wd2));
    free(wc);
    free(wa2);
    free(wd2);
#else
    assert(!unlink(copied_path));
    assert(!rmdir(other_assets));
    assert(!rmdir(other_directory));
#endif
    free(copied_path);
    free(other_assets);
    free(other_directory);
    app_close(&app);
    const char *files[] = {"1/jot.md", "AGENTS.md", "CLAUDE.md", name, NULL};
    for (unsigned i = 0; files[i]; ++i) {
        snprintf(relative, sizeof(relative), "%s%s", files[i] == name ? "1/" : "", files[i]);
        char full[1400];
        snprintf(full, sizeof(full), "%s/%s", directory, relative);
#ifdef _WIN32
        wchar_t *w = windows_path(full);
        DeleteFileW(w);
        free(w);
#else
        unlink(full);
#endif
    }
    for (unsigned i = 0; i < 2; ++i) {
        char full[1400];
        snprintf(full, sizeof(full), "%s/1/%s.%s", directory, name, i ? "tsv" : "txt");
#ifdef _WIN32
        wchar_t *w = windows_path(full);
        DeleteFileW(w);
        free(w);
#else
        unlink(full);
#endif
    }
    char assets[1200], first[1200];
    snprintf(assets, sizeof(assets), "%s/1/assets", directory);
    snprintf(first, sizeof(first), "%s/1", directory);
#ifdef _WIN32
    wchar_t *wa = windows_path(assets), *wf = windows_path(first), *wd = windows_path(directory);
    RemoveDirectoryW(wa);
    RemoveDirectoryW(wf);
    RemoveDirectoryW(wd);
    free(wa);
    free(wf);
    free(wd);
#else
    rmdir(assets);
    rmdir(first);
    rmdir(directory);
#endif
    puts("Bounded image codec, assets, inline rendering/scales, OCR search and undo passed");
}
