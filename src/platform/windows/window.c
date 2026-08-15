#include "win32.h"
#include "app.h"
#include "platform.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

static const wchar_t window_class[] = L"TinyViWindow";
struct windows_window {
    struct app *app;
    HWND handle;
    UINT dpi;
    wchar_t high_surrogate;
    unsigned high_repeat;
};

static void update_geometry(struct windows_window *window)
{
    RECT client;
    if (!GetClientRect(window->handle, &client)) return;
    window->app->scale120 = (uint32_t)MulDiv((int)window->dpi, 120, 96);
    window->app->width = MulDiv(client.right, 96, (int)window->dpi);
    window->app->height = MulDiv(client.bottom, 96, (int)window->dpi);
    window->app->dirty = true;
}

static void changed(struct windows_window *window)
{
    if (!window->app->running) DestroyWindow(window->handle);
    else if (window->app->dirty) InvalidateRect(window->handle, NULL, FALSE);
}

static void codepoint(struct windows_window *window, uint32_t cp, unsigned repeat)
{
    char text[5] = {0};
    if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) cp = 0xfffd;
    enum editor_key key = KEY_TEXT;
    if (cp == 27) key = KEY_ESCAPE;
    else if (cp == 13) key = KEY_ENTER;
    else if (cp == 8) key = KEY_BACKSPACE;
    else if (cp == 9) key = KEY_TAB;
    else if (cp < 32 || cp == 127) return;
    else if (cp < 0x80) text[0] = (char)cp;
    else if (cp < 0x800) {
        text[0] = (char)(0xc0 | (cp >> 6)); text[1] = (char)(0x80 | (cp & 63));
    } else if (cp < 0x10000) {
        text[0] = (char)(0xe0 | (cp >> 12)); text[1] = (char)(0x80 | ((cp >> 6) & 63));
        text[2] = (char)(0x80 | (cp & 63));
    } else {
        text[0] = (char)(0xf0 | (cp >> 18)); text[1] = (char)(0x80 | ((cp >> 12) & 63));
        text[2] = (char)(0x80 | ((cp >> 6) & 63)); text[3] = (char)(0x80 | (cp & 63));
    }
    for (unsigned i = 0; i < repeat && window->app->running; ++i)
        app_input(window->app, key, text);
}

static void character(struct windows_window *window, wchar_t unit, unsigned repeat)
{
    if (window->high_surrogate) {
        wchar_t high = window->high_surrogate;
        unsigned prior_repeat = window->high_repeat;
        window->high_surrogate = 0;
        if (unit >= 0xdc00 && unit <= 0xdfff) {
            codepoint(window, 0x10000 + (((uint32_t)high - 0xd800) << 10) + unit - 0xdc00, repeat);
            return;
        }
        codepoint(window, 0xfffd, prior_repeat);
    }
    if (unit >= 0xd800 && unit <= 0xdbff) {
        window->high_surrogate = unit; window->high_repeat = repeat;
    } else codepoint(window, unit >= 0xdc00 && unit <= 0xdfff ? 0xfffd : unit, repeat);
}

static bool paint_window(struct windows_window *window, HDC dc)
{
    RECT rect;
    if (!GetClientRect(window->handle, &rect)) return false;
    int width = rect.right, height = rect.bottom;
    if (width <= 0 || height <= 0) return true;
    if (width > INT_MAX / 4 / height) return false;
    size_t bytes = (size_t)width * (size_t)height * 4;
    uint32_t *pixels = platform_allocate_zeroed(bytes);
    if (!pixels) return false;
    bool ok = app_paint(window->app, pixels, width, height, width);
    if (ok) {
        BITMAPINFO info = {0};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height; /* Top-down; no bitmap flip or scaling. */
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        int result = StretchDIBits(dc, 0, 0, width, height, 0, 0, width, height,
                                  pixels, &info, DIB_RGB_COLORS, SRCCOPY);
        ok = result != (int)GDI_ERROR;
        if (ok) ok = GdiFlush() != 0;
    }
    platform_release(pixels, bytes);
    if (ok) window->app->dirty = false;
    return ok;
}

static LRESULT CALLBACK window_proc(HWND handle, UINT message, WPARAM wparam, LPARAM lparam)
{
    struct windows_window *window = (void *)GetWindowLongPtrW(handle, GWLP_USERDATA);
    if (message == WM_NCCREATE) {
        window = ((CREATESTRUCTW *)lparam)->lpCreateParams;
        window->handle = handle;
        SetWindowLongPtrW(handle, GWLP_USERDATA, (LONG_PTR)window);
    }
    if (!window) return DefWindowProcW(handle, message, wparam, lparam);
    switch (message) {
    case WM_CREATE:
        window->dpi = GetDpiForWindow(handle);
        if (!window->dpi) window->dpi = 96;
        update_geometry(window);
        return 0;
    case WM_SIZE:
        if (wparam != SIZE_MINIMIZED) { update_geometry(window); InvalidateRect(handle, NULL, FALSE); }
        return 0;
    case WM_DPICHANGED: {
        UINT dpi = LOWORD(wparam);
        if (!dpi) return 0;
        window->dpi = dpi;
        RECT *suggested = (RECT *)lparam;
        SetWindowPos(handle, NULL, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOACTIVATE | SWP_NOZORDER);
        update_geometry(window);
        InvalidateRect(handle, NULL, FALSE);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(handle, &paint);
        bool ok = dc && paint_window(window, dc);
        EndPaint(handle, &paint);
        if (!ok) {
            fprintf(stderr, "Could not render Windows client area\n");
            window->app->failed = true; window->app->running = false;
            DestroyWindow(handle);
        }
        return 0;
    }
    case WM_KEYDOWN: {
        enum editor_key key;
        switch (wparam) {
        case VK_LEFT: key = KEY_LEFT; break;
        case VK_RIGHT: key = KEY_RIGHT; break;
        case VK_UP: key = KEY_UP; break;
        case VK_DOWN: key = KEY_DOWN; break;
        case VK_DELETE: key = KEY_DELETE; break;
        default: return DefWindowProcW(handle, message, wparam, lparam);
        }
        unsigned repeat = LOWORD(lparam); if (!repeat) repeat = 1;
        for (unsigned i = 0; i < repeat && window->app->running; ++i) app_input(window->app, key, "");
        changed(window);
        return 0;
    }
    case WM_CHAR: {
        unsigned repeat = LOWORD(lparam); if (!repeat) repeat = 1;
        character(window, (wchar_t)wparam, repeat);
        changed(window);
        return 0;
    }
    case WM_UNICHAR:
        if (wparam == UNICODE_NOCHAR) return TRUE;
        codepoint(window, (uint32_t)wparam, 1);
        changed(window);
        return 0;
    case WM_KILLFOCUS:
        window->high_surrogate = 0;
        return 0;
    case WM_CLOSE:
        app_request_close(window->app);
        changed(window);
        return 0;
    case WM_DESTROY:
        window->handle = NULL;
        PostQuitMessage(0);
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(handle, GWLP_USERDATA, 0);
        break;
    default: break;
    }
    return DefWindowProcW(handle, message, wparam, lparam);
}

static bool open_window(struct windows_window *window, struct app *app, int show)
{
    *window = (struct windows_window){.app = app, .dpi = 96};
    /* Windows 10+; Wine may fall back to system DPI awareness. */
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        SetProcessDPIAware();
    HINSTANCE instance = GetModuleHandleW(NULL);
    WNDCLASSW klass = {0};
    klass.lpfnWndProc = window_proc;
    klass.hInstance = instance;
    klass.hCursor = LoadCursorW(NULL, IDC_IBEAM);
    klass.lpszClassName = window_class;
    if (!RegisterClassW(&klass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    UINT dpi = GetDpiForSystem(); if (!dpi) dpi = 96;
    RECT size = {0, 0, MulDiv(app->width, (int)dpi, 96), MulDiv(app->height, (int)dpi, 96)};
    if (!AdjustWindowRectExForDpi(&size, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi)) return false;
    HWND handle = CreateWindowExW(0, window_class, L"Tiny vi - Windows / CPU", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, size.right - size.left, size.bottom - size.top,
        NULL, NULL, instance, window);
    if (!handle) return false;
    ShowWindow(handle, show);
    UpdateWindow(handle);
    return window->handle != NULL;
}

int platform_run(struct app *app)
{
    struct windows_window window;
    if (!open_window(&window, app, SW_SHOWNORMAL)) {
        char error[512]; windows_error(error, sizeof(error), "Create window", GetLastError());
        fprintf(stderr, "%s\n", error);
        return EXIT_FAILURE;
    }
    MSG message;
    int result;
    while ((result = (int)GetMessageW(&message, NULL, 0, 0)) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (result < 0) app->failed = true;
    if (window.handle) DestroyWindow(window.handle);
    UnregisterClassW(window_class, GetModuleHandleW(NULL));
    return app->failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
