/* Included by window.c: native clipboard and shell actions share its HWND. */
#include "media.h"
#include <shellapi.h>
#include <string.h>
struct tiny_drop {
    DWORD pFiles;
    POINT pt;
    BOOL fNC, fWide;
};
static HGLOBAL clipboard_bytes(const void *bytes, size_t length)
{
    HGLOBAL block = GlobalAlloc(GMEM_MOVEABLE, length ? length : 1);
    if (!block)
        return NULL;
    void *p = GlobalLock(block);
    if (!p) {
        GlobalFree(block);
        return NULL;
    }
    memcpy(p, bytes, length);
    GlobalUnlock(block);
    return block;
}
static HGLOBAL clipboard_text(const char *text)
{
    if (!text)
        return NULL;
    size_t n = strlen(text);
    if (n > 16 * 1024 * 1024)
        return NULL;
    char *dos = malloc(n * 2 + 1);
    if (!dos)
        return NULL;
    size_t j = 0;
    for (size_t i = 0; i < n; ++i) {
        if (text[i] == '\n' && (!i || text[i - 1] != '\r'))
            dos[j++] = '\r';
        dos[j++] = text[i];
    }
    dos[j] = 0;
    wchar_t *w = windows_utf16(dos);
    free(dos);
    if (!w)
        return NULL;
    HGLOBAL block = clipboard_bytes(w, (wcslen(w) + 1) * sizeof(*w));
    free(w);
    return block;
}
static void import_drop(struct app *app, HGLOBAL handle)
{
    SIZE_T bytes = GlobalSize(handle);
    const struct tiny_drop *drop = GlobalLock(handle);
    if (!drop)
        return;
    if (bytes >= sizeof(*drop) && drop->pFiles < bytes && drop->fWide &&
        drop->pFiles % sizeof(wchar_t) == 0) {
        const wchar_t *p = (const wchar_t *)((const unsigned char *)drop + drop->pFiles);
        size_t left = (bytes - drop->pFiles) / sizeof(wchar_t);
        while (left && *p) {
            size_t n = 0;
            while (n < left && p[n])
                ++n;
            if (n == left)
                break;
            char *path = windows_utf8(p);
            if (path) {
                media_import_file(app, path);
                free(path);
            }
            p += n + 1;
            left -= n + 1;
        }
    }
    GlobalUnlock(handle);
}
static bool paste_dib(struct app *app, HGLOBAL handle)
{
    SIZE_T bytes = GlobalSize(handle);
    const unsigned char *data = GlobalLock(handle);
    if (!data)
        return false;
    bool ok = false;
    if (bytes >= sizeof(BITMAPINFOHEADER)) {
        BITMAPINFOHEADER h;
        memcpy(&h, data, sizeof(h));
        if (h.biSize >= 40 && h.biSize <= bytes && h.biWidth > 0 && h.biHeight &&
            h.biHeight != INT_MIN && h.biPlanes == 1 &&
            (h.biBitCount == 24 || h.biBitCount == 32) && h.biCompression == BI_RGB) {
            int height = h.biHeight < 0 ? -h.biHeight : h.biHeight;
            uint64_t pixels = (uint64_t)h.biWidth * (unsigned)height;
            uint64_t stride = (((uint64_t)h.biWidth * h.biBitCount + 31) / 32) * 4;
            if (pixels <= 8 * 1024 * 1024 && stride * (unsigned)height <= bytes - h.biSize) {
                unsigned char *rgba = malloc((size_t)pixels * 4);
                if (rgba) {
                    for (int y = 0; y < height; ++y)
                        for (int x = 0; x < h.biWidth; ++x) {
                            const unsigned char *p =
                                data + h.biSize +
                                (size_t)(h.biHeight > 0 ? height - y - 1 : y) * (size_t)stride +
                                (size_t)x * (h.biBitCount / 8);
                            unsigned char *out = rgba + ((size_t)y * h.biWidth + x) * 4;
                            out[0] = p[2];
                            out[1] = p[1];
                            out[2] = p[0];
                            out[3] = 255;
                        }
                    size_t n;
                    unsigned char *png = image_png(rgba, h.biWidth, height, &n);
                    free(rgba);
                    if (png) {
                        ok = media_paste_image(app, png, n);
                        free(png);
                    }
                }
            }
        }
    }
    GlobalUnlock(handle);
    return ok;
}
static void windows_open(const char *target)
{
    if (!target)
        return;
    HMODULE library = LoadLibraryW(L"shell32.dll");
    if (!library)
        return;
    typedef HINSTANCE(WINAPI * execute_fn)(HWND, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, INT);
    execute_fn execute = (execute_fn)(void *)GetProcAddress(library, "ShellExecuteW");
    wchar_t *w = windows_utf16(target);
    if (execute && w)
        execute(NULL, L"open", w, NULL, NULL, SW_SHOWNORMAL);
    free(w);
    FreeLibrary(library);
}
static void windows_service(struct app *app, unsigned requests, void *context)
{
    struct windows_window *window = context;
    struct editor *ed = &app->editor;
    if (requests & (ED_OPEN | ED_OPEN_DIRECTORY)) {
        char *target = requests & ED_OPEN_DIRECTORY
                           ? (app->directory ? session_path(app, "") : NULL)
                           : media_target(app);
        windows_open(target);
        free(target);
    }
    if (requests & (ED_COPY | ED_COPY_OCR)) {
        HGLOBAL text = NULL, png = NULL, dib = NULL, fragment = NULL;
        size_t n = 0;
        char *ocr = NULL;
        const char *image_path = requests & ED_COPY_OCR ? NULL : app->clipboard_image;
        if (image_path) {
            char *data = media_clipboard_image(app, &n);
            if (data) {
                png = clipboard_bytes(data, n);
                struct image image = {0};
                if (image_decode(&image, (unsigned char *)data, n)) {
                    size_t bytes =
                        sizeof(BITMAPINFOHEADER) + (size_t)image.width * image.height * 4;
                    dib = GlobalAlloc(GMEM_MOVEABLE, bytes);
                    if (dib) {
                        unsigned char *out = GlobalLock(dib);
                        if (out) {
                            BITMAPINFOHEADER header = {.biSize = sizeof(header),
                                                       .biWidth = image.width,
                                                       .biHeight = -image.height,
                                                       .biPlanes = 1,
                                                       .biBitCount = 32,
                                                       .biCompression = BI_RGB};
                            memcpy(out, &header, sizeof(header));
                            for (size_t i = 0; i < (size_t)image.width * image.height; ++i) {
                                out[sizeof(header) + i * 4] = image.rgba[i * 4 + 2];
                                out[sizeof(header) + i * 4 + 1] = image.rgba[i * 4 + 1];
                                out[sizeof(header) + i * 4 + 2] = image.rgba[i * 4];
                                out[sizeof(header) + i * 4 + 3] = 255;
                            }
                            GlobalUnlock(dib);
                        } else {
                            GlobalFree(dib);
                            dib = NULL;
                        }
                    }
                    image_free(&image);
                }
                free(data);
            }
        } else {
            if (requests & ED_COPY_OCR)
                ocr = media_ocr_text(app, &n);
            text = clipboard_text(ocr ? ocr : requests & ED_COPY_OCR ? "" : ed->register_text);
            free(ocr);
        }
        if (!(requests & ED_COPY_OCR)) {
            /* An image is offered only as an image, since apps offered text as well paste
             * the text; mib itself uses the fragment. */
            if (!text && !png && !dib)
                text = clipboard_text(ed->register_text);
            size_t fragment_length;
            char *bytes = media_fragment(app, &fragment_length);
            if (bytes) {
                fragment = clipboard_bytes(bytes, fragment_length);
                free(bytes);
            }
        }
        if ((text || png || dib || fragment) && OpenClipboard(window->handle)) {
            EmptyClipboard();
            if (fragment && SetClipboardData(RegisterClipboardFormatW(L"Mib.Fragment"), fragment))
                fragment = NULL;
            if (text && SetClipboardData(CF_UNICODETEXT, text))
                text = NULL;
            if (png && SetClipboardData(RegisterClipboardFormatW(L"PNG"), png))
                png = NULL;
            if (dib && SetClipboardData(CF_DIB, dib))
                dib = NULL;
            CloseClipboard();
        }
        if (fragment)
            GlobalFree(fragment);
        if (text)
            GlobalFree(text);
        if (png)
            GlobalFree(png);
        if (dib)
            GlobalFree(dib);
    }
    if (requests & ED_PASTE) {
        bool pasted = false;
        if (OpenClipboard(window->handle)) {
            HGLOBAL handle = GetClipboardData(RegisterClipboardFormatW(L"PNG"));
            if (handle) {
                SIZE_T n = GlobalSize(handle);
                void *data = GlobalLock(handle);
                if (data && n <= 32 * 1024 * 1024)
                    pasted = media_paste_image(app, data, n);
                if (data)
                    GlobalUnlock(handle);
            }
            if (!pasted && (handle = GetClipboardData(CF_DIB)))
                pasted = paste_dib(app, handle);
            if (!pasted && (handle = GetClipboardData(RegisterClipboardFormatW(L"Mib.Fragment")))) {
                SIZE_T length = GlobalSize(handle);
                const unsigned char *bytes = GlobalLock(handle);
                if (bytes && length <= 32 * 1024 * 1024)
                    pasted = media_paste_fragment(app, bytes, length);
                if (bytes)
                    GlobalUnlock(handle);
            }
            if (!pasted && (handle = GetClipboardData(CF_HDROP))) {
                import_drop(app, handle);
                pasted = true;
            }
            if (!pasted && (handle = GetClipboardData(CF_UNICODETEXT))) {
                SIZE_T n = GlobalSize(handle) / sizeof(wchar_t);
                const wchar_t *w = GlobalLock(handle);
                if (w) {
                    size_t length = 0;
                    while (length < n && w[length])
                        ++length;
                    if (length < n && length <= 16 * 1024 * 1024) {
                        char *text = windows_utf8(w);
                        if (text) {
                            media_paste_text(app, text, strlen(text));
                            free(text);
                            pasted = true;
                        }
                    }
                    GlobalUnlock(handle);
                }
            }
            CloseClipboard();
        }
        if (!pasted)
            editor_put(ed, ed->register_text, ed->register_length, ed->register_linewise,
                       ed->put_after, ed->put_count);
    }
}
