/* Use the real Win32 format/memory code with a private clipboard store. The
   Wine desktop/user clipboard is never read, cleared or replaced by this test. */
#include "../src/platform/windows/win32.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct format {
    UINT id;
    HANDLE data;
};
static struct format formats[8];
static unsigned format_count;
static bool opened;
static BOOL WINAPI private_open(HWND window)
{
    (void)window;
    assert(!opened);
    opened = true;
    return TRUE;
}
static BOOL WINAPI private_close(void)
{
    assert(opened);
    opened = false;
    return TRUE;
}
static BOOL WINAPI private_empty(void)
{
    assert(opened);
    for (unsigned i = 0; i < format_count; ++i)
        GlobalFree(formats[i].data);
    format_count = 0;
    return TRUE;
}
static HANDLE WINAPI private_set(UINT id, HANDLE data)
{
    assert(opened && format_count < 8);
    formats[format_count++] = (struct format){id, data};
    return data;
}
static HANDLE WINAPI private_get(UINT id)
{
    assert(opened);
    for (unsigned i = 0; i < format_count; ++i)
        if (formats[i].id == id)
            return formats[i].data;
    return NULL;
}
#define OpenClipboard private_open
#define CloseClipboard private_close
#define EmptyClipboard private_empty
#define SetClipboardData private_set
#define GetClipboardData private_get
#include "../src/platform/windows/window.c"
static void blank(struct app *app)
{
    editor_change_end(&app->editor);
    assert(editor_replace(&app->editor, 0, app->editor.length, "", 0));
    app->editor.cursor = 0;
    app->editor.mode = NORMAL;
}
static void keys(struct app *app, const char *s)
{
    while (*s) {
        char c[2] = {*s++, 0};
        app_input(app, KEY_TEXT, c);
    }
}
int main(void)
{
    _putenv("JOT_OCR=0");
    char temp[MAX_PATH], directory[1024];
    assert(GetTempPathA(MAX_PATH, temp));
    snprintf(directory, sizeof(directory), "%sjot-private-clipboard-%lu-%lu", temp,
             (unsigned long)GetCurrentProcessId(), (unsigned long)GetTickCount());
    assert(session_mkdir(directory));
    struct app app;
    assert(app_open(&app, NULL));
    app.directory = malloc(strlen(directory) + 1);
    assert(app.directory);
    strcpy(app.directory, directory);
    struct windows_window window;
    assert(open_window(&window, &app, SW_HIDE));
    keys(&app, "iAlpha beta");
    app_input(&app, KEY_ESCAPE, "");
    keys(&app, "yy");
    assert(private_open(NULL));
    HGLOBAL handle = private_get(CF_UNICODETEXT);
    assert(handle);
    const wchar_t *w = GlobalLock(handle);
    assert(w && !wcscmp(w, L"Alpha beta\r\n"));
    GlobalUnlock(handle);
    private_close();
    blank(&app);
    keys(&app, "p");
    assert(!strcmp(app.editor.text, "Alpha beta\n"));
    const unsigned char rgba[] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 0, 255};
    size_t n;
    unsigned char *png = image_png(rgba, 2, 2, &n);
    assert(png);
    blank(&app);
    assert(media_paste_image(&app, png, n));
    free(png);
    keys(&app, "yy");
    assert(private_open(NULL));
    UINT png_format = RegisterClipboardFormatW(L"PNG");
    handle = private_get(png_format);
    assert(handle && private_get(CF_DIB));
    unsigned char *bytes = GlobalLock(handle);
    struct image image = {0};
    assert(bytes && image_decode(&image, bytes, GlobalSize(handle)));
    assert(!memcmp(image.rgba, rgba, sizeof(rgba)));
    image_free(&image);
    GlobalUnlock(handle);
    private_close();
    blank(&app);
    keys(&app, "p");
    assert(media_at(&app, app.editor.cursor));
    /* Native DIB-only applications use the same bounded image path. */
    for (unsigned i = 0; i < format_count; ++i)
        if (formats[i].id == png_format) {
            GlobalFree(formats[i].data);
            formats[i] = formats[--format_count];
            break;
        }
    blank(&app);
    keys(&app, "p");
    assert(media_at(&app, app.editor.cursor));
    assert(editor_replace(&app.editor, 0, 0, "before\n", 7));
    assert(editor_replace(&app.editor, app.editor.length, app.editor.length, "after\n", 6));
    app.editor.cursor = 0;
    keys(&app, "VGy");
    assert(private_open(NULL));
    assert(private_get(RegisterClipboardFormatW(L"Jot.Fragment")) && !private_get(png_format));
    private_close();
    blank(&app);
    keys(&app, "p");
    assert(strstr(app.editor.text, "before\n") && strstr(app.editor.text, "after\n") &&
           strstr(app.editor.text, "![]("));
    char file[1200];
    snprintf(file, sizeof(file), "%s/drop.txt", directory);
    assert(session_write_new(file, "dropped text\n", 13));
    wchar_t *wide = windows_utf16(file);
    assert(wide);
    size_t path_bytes = (wcslen(wide) + 2) * sizeof(wchar_t),
           total = sizeof(struct tiny_drop) + path_bytes;
    HGLOBAL drop = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, total);
    assert(drop);
    struct tiny_drop *d = GlobalLock(drop);
    assert(d);
    d->pFiles = sizeof(*d);
    d->fWide = TRUE;
    memcpy((unsigned char *)d + sizeof(*d), wide, path_bytes - sizeof(wchar_t));
    GlobalUnlock(drop);
    free(wide);
    assert(private_open(NULL));
    private_empty();
    private_set(CF_HDROP, drop);
    private_close();
    blank(&app);
    keys(&app, "p");
    assert(!strcmp(app.editor.text, "dropped text\n"));
    assert(private_open(NULL));
    private_empty();
    private_close();
    DestroyWindow(window.handle);
    app_close(&app);
    char pattern[1400];
    snprintf(pattern, sizeof(pattern), "%s/assets/*", directory);
    wchar_t *wp = windows_path(pattern);
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW(wp, &found);
    free(wp);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                char *name = windows_utf8(found.cFileName);
                snprintf(pattern, sizeof(pattern), "%s/assets/%s", directory, name);
                free(name);
                wp = windows_path(pattern);
                assert(DeleteFileW(wp));
                free(wp);
            }
        } while (FindNextFileW(search, &found));
        FindClose(search);
    }
    snprintf(pattern, sizeof(pattern), "%s/assets", directory);
    wp = windows_path(pattern);
    assert(RemoveDirectoryW(wp));
    free(wp);
    wp = windows_path(file);
    assert(DeleteFileW(wp));
    free(wp);
    wp = windows_path(directory);
    assert(RemoveDirectoryW(wp));
    free(wp);
    puts("Private Windows clipboard: Unicode/CRLF, PNG, DIB and dropped-file serialization passed");
}
