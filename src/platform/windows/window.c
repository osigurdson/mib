#include "app.h"
#include "media.h"
#include "platform.h"
#include "session.h"
#include "win32.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

static const wchar_t window_class[] = L"JotWindow";
struct windows_window {
    struct app *app;
    HWND handle;
    UINT dpi;
    wchar_t high_surrogate;
    unsigned high_repeat;
    bool dragging;
    /* An Alt+digit page switch: page_char drops its WM_SYSCHAR, and alt_code drops the Alt
     * code character a keypad digit types when Alt is released. */
    bool page_char, alt_code;
};

#include "clipboard.inc"

static void update_geometry(struct windows_window *window)
{
    RECT client;
    if (!GetClientRect(window->handle, &client))
        return;
    window->app->scale120 = (uint32_t)MulDiv((int)window->dpi, 120, 96);
    window->app->width = MulDiv(client.right, 96, (int)window->dpi);
    window->app->height = MulDiv(client.bottom, 96, (int)window->dpi);
    window->app->dirty = true;
}

static void changed(struct windows_window *window)
{
    if (!window->app->running) {
        DestroyWindow(window->handle);
        return;
    }
    uint64_t now = session_now(), deadline = app_deadline(window->app);
    if (window->app->dirty) {
        if (now >= window->app->redraw_at)
            InvalidateRect(window->handle, NULL, FALSE);
        else if (!deadline || window->app->redraw_at < deadline)
            deadline = window->app->redraw_at;
    }
    if (deadline) {
        uint64_t remaining = deadline > now ? deadline - now : 1;
        SetTimer(window->handle, 1, (UINT)remaining, NULL);
    } else
        KillTimer(window->handle, 1);
}

static void codepoint(struct windows_window *window, uint32_t cp, unsigned repeat)
{
    char text[5] = {0};
    if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
        cp = 0xfffd;
    enum editor_key key = KEY_TEXT;
    if (cp == 27)
        key = KEY_ESCAPE;
    else if (cp == 13)
        key = KEY_ENTER;
    else if (cp == 8)
        key = KEY_BACKSPACE;
    else if (cp == 9)
        key = GetKeyState(VK_SHIFT) < 0 ? KEY_SHIFT_TAB : KEY_TAB;
    else if (cp == 2)
        key = KEY_CTRL_B;
    else if (cp == 3)
        key = GetKeyState(VK_SHIFT) < 0 ? KEY_COPY_IMAGE_TEXT : KEY_CTRL_C;
    else if (cp == 4)
        key = KEY_CTRL_D;
    else if (cp == 5)
        key = KEY_CTRL_E;
    else if (cp == 6)
        key = KEY_CTRL_F;
    else if (cp == 15)
        key = KEY_CTRL_O;
    else if (cp == 17)
        key = KEY_CTRL_Q;
    else if (cp == 18)
        key = KEY_CTRL_R;
    else if (cp == 19)
        key = KEY_CTRL_S;
    else if (cp == 21)
        key = KEY_CTRL_U;
    else if (cp == 22)
        key = KEY_CTRL_V;
    else if (cp == 23)
        key = KEY_CTRL_W;
    else if (cp == 25)
        key = KEY_CTRL_Y;
    else if (cp < 32 || cp == 127)
        return;
    else if (cp < 0x80)
        text[0] = (char)cp;
    else if (cp < 0x800) {
        text[0] = (char)(0xc0 | (cp >> 6));
        text[1] = (char)(0x80 | (cp & 63));
    } else if (cp < 0x10000) {
        text[0] = (char)(0xe0 | (cp >> 12));
        text[1] = (char)(0x80 | ((cp >> 6) & 63));
        text[2] = (char)(0x80 | (cp & 63));
    } else {
        text[0] = (char)(0xf0 | (cp >> 18));
        text[1] = (char)(0x80 | ((cp >> 12) & 63));
        text[2] = (char)(0x80 | ((cp >> 6) & 63));
        text[3] = (char)(0x80 | (cp & 63));
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
        window->high_surrogate = unit;
        window->high_repeat = repeat;
    } else
        codepoint(window, unit >= 0xdc00 && unit <= 0xdfff ? 0xfffd : unit, repeat);
}

static bool paint_window(struct windows_window *window, HDC dc)
{
    RECT rect;
    if (!GetClientRect(window->handle, &rect))
        return false;
    int width = rect.right, height = rect.bottom;
    if (width <= 0 || height <= 0)
        return true;
    if (width > INT_MAX / 4 / height)
        return false;
    size_t bytes = (size_t)width * (size_t)height * 4;
    uint32_t *pixels = platform_allocate_zeroed(bytes);
    if (!pixels)
        return false;
    bool ok = app_paint(window->app, pixels, width, height, width);
    if (ok) {
        BITMAPINFO info = {0};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height; /* Top-down; no bitmap flip or scaling. */
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        int result = StretchDIBits(dc, 0, 0, width, height, 0, 0, width, height, pixels, &info,
                                   DIB_RGB_COLORS, SRCCOPY);
        ok = result != (int)GDI_ERROR;
        if (ok)
            ok = GdiFlush() != 0;
    }
    platform_release(pixels, bytes);
    if (ok)
        window->app->dirty = false;
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
    if (!window)
        return DefWindowProcW(handle, message, wparam, lparam);
    switch (message) {
    case WM_CREATE:
        window->dpi = GetDpiForWindow(handle);
        if (!window->dpi)
            window->dpi = 96;
        update_geometry(window);
        return 0;
    case WM_SIZE:
        if (wparam != SIZE_MINIMIZED) {
            window->app->maximized = wparam == SIZE_MAXIMIZED;
            update_geometry(window);
            if (wparam != SIZE_MAXIMIZED) {
                window->app->restore_width = window->app->width;
                window->app->restore_height = window->app->height;
            }
            InvalidateRect(handle, NULL, FALSE);
        }
        return 0;
    case WM_DPICHANGED: {
        UINT dpi = LOWORD(wparam);
        if (!dpi)
            return 0;
        window->dpi = dpi;
        RECT *suggested = (RECT *)lparam;
        SetWindowPos(handle, NULL, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOACTIVATE | SWP_NOZORDER);
        update_geometry(window);
        InvalidateRect(handle, NULL, FALSE);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(handle, &paint);
        uint64_t started = session_now();
        bool due = started >= window->app->redraw_at;
        bool ok = dc != NULL;
        if (ok && due) {
            ok = paint_window(window, dc);
            if (ok)
                window->app->redraw_at = started + APP_FRAME_INTERVAL_MS;
        } else if (!due)
            window->app->dirty = true;
        EndPaint(handle, &paint);
        if (!ok) {
            fprintf(stderr, "Could not render Windows client area\n");
            window->app->failed = true;
            window->app->running = false;
            DestroyWindow(handle);
        } else
            changed(window);
        return 0;
    }
    case WM_RBUTTONDOWN:
        app_context(window->app, MulDiv((short)LOWORD(lparam), 96, (int)window->dpi),
                    MulDiv((short)HIWORD(lparam), 96, (int)window->dpi));
        changed(window);
        return 0;
    case WM_LBUTTONDBLCLK:
        app_pointer(window->app, MulDiv((short)LOWORD(lparam), 96, (int)window->dpi),
                    MulDiv((short)HIWORD(lparam), 96, (int)window->dpi), false);
        if (window->app->image_drag) {
            /* A double click on a corner handle starts a resize, not Open Image. */
            window->dragging = true;
            SetCapture(handle);
        } else
            windows_service(window->app, ED_OPEN, window);
        changed(window);
        return 0;
    case WM_DROPFILES:
        import_drop(window->app, (HGLOBAL)wparam);
        GlobalFree((HGLOBAL)wparam);
        changed(window);
        return 0;
    case WM_MOUSEWHEEL:
        app_scroll(window->app, (short)HIWORD(wparam) > 0 ? -3 : 3);
        changed(window);
        return 0;
    case WM_LBUTTONDOWN: {
        bool menu = window->app->context_menu;
        window->dragging = true;
        SetCapture(handle);
        app_pointer(window->app, MulDiv((short)LOWORD(lparam), 96, (int)window->dpi),
                    MulDiv((short)HIWORD(lparam), 96, (int)window->dpi), (wparam & MK_SHIFT) != 0);
        if (!window->app->image_drag && (menu || GetKeyState(VK_CONTROL) < 0)) {
            if (!menu)
                windows_service(window->app, ED_OPEN, window);
            window->dragging = false;
            ReleaseCapture();
        }
        changed(window);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (window->dragging) {
            app_pointer(window->app, MulDiv((short)LOWORD(lparam), 96, (int)window->dpi),
                        MulDiv((short)HIWORD(lparam), 96, (int)window->dpi), true);
            changed(window);
        }
        return 0;
    case WM_LBUTTONUP:
        window->dragging = false;
        ReleaseCapture();
        if (window->app->image_drag) {
            app_pointer_release(window->app);
            changed(window);
        }
        return 0;
    case WM_CAPTURECHANGED:
        window->dragging = false;
        if (window->app->image_drag) {
            app_pointer_release(window->app);
            changed(window);
        }
        break;
    case WM_SETCURSOR:
        if (LOWORD(lparam) == HTCLIENT) {
            POINT point;
            GetCursorPos(&point);
            ScreenToClient(handle, &point);
            int corner = window->app->image_drag
                             ? window->app->drag_corner
                             : app_image_corner(window->app, MulDiv(point.x, 96, (int)window->dpi),
                                                MulDiv(point.y, 96, (int)window->dpi));
            if (corner >= 0) {
                SetCursor(LoadCursorW(NULL, corner == 0 || corner == 3 ? IDC_SIZENWSE : IDC_SIZENESW));
                return TRUE;
            }
        }
        break;
    case WM_KEYDOWN: {
        enum editor_key key;
        if (GetKeyState(VK_CONTROL) < 0 &&
            (wparam == VK_OEM_PLUS || wparam == VK_ADD || wparam == VK_OEM_MINUS ||
             wparam == VK_SUBTRACT || wparam == '0')) {
            app_input(window->app,
                      wparam == '0'                                     ? KEY_ZOOM_RESET
                      : wparam == VK_OEM_MINUS || wparam == VK_SUBTRACT ? KEY_ZOOM_OUT
                                                                        : KEY_ZOOM_IN,
                      "");
            changed(window);
            return 0;
        }
        switch (wparam) {
        case VK_LEFT:
            key = KEY_LEFT;
            break;
        case VK_RIGHT:
            key = KEY_RIGHT;
            break;
        case VK_UP:
            key = KEY_UP;
            break;
        case VK_DOWN:
            key = KEY_DOWN;
            break;
        case VK_HOME:
            key = KEY_HOME;
            break;
        case VK_END:
            key = KEY_END;
            break;
        case VK_PRIOR:
            key = KEY_PAGE_UP;
            break;
        case VK_NEXT:
            key = KEY_PAGE_DOWN;
            break;
        case VK_DELETE:
            key = KEY_DELETE;
            break;
        default:
            return DefWindowProcW(handle, message, wparam, lparam);
        }
        unsigned repeat = LOWORD(lparam);
        if (!repeat)
            repeat = 1;
        for (unsigned i = 0; i < repeat && window->app->running; ++i)
            app_input(window->app, key, "");
        changed(window);
        return 0;
    }
    case WM_SYSKEYDOWN: {
        /* Alt+1 to Alt+9 switch pages and Alt+0 lists them, from the number row or the keypad
         * (Num Lock on or off; keypad navigation keys are the ones without the extended-key
         * flag). */
        static const WPARAM navigation[] = {VK_END,   VK_DOWN, VK_NEXT, VK_LEFT, VK_CLEAR,
                                            VK_RIGHT, VK_HOME, VK_UP,   VK_PRIOR};
        char page[2] = {0};
        bool keypad = false;
        if (wparam >= '0' && wparam <= '9')
            page[0] = (char)wparam;
        else if (wparam == VK_NUMPAD0 || (wparam == VK_INSERT && !(lparam & (1 << 24)))) {
            page[0] = '0';
            keypad = true;
        } else if (wparam >= VK_NUMPAD1 && wparam <= VK_NUMPAD9) {
            page[0] = (char)('1' + (wparam - VK_NUMPAD1));
            keypad = true;
        } else if (!(lparam & (1 << 24)))
            for (unsigned i = 0; i < 9; ++i)
                if (wparam == navigation[i]) {
                    page[0] = (char)('1' + i);
                    keypad = true;
                }
        if (!page[0] || GetKeyState(VK_CONTROL) < 0)
            break;
        window->page_char = true;
        window->alt_code |= keypad;
        app_input(window->app, KEY_PAGE, page);
        changed(window);
        return 0;
    }
    case WM_SYSCHAR:
        if (window->page_char) {
            window->page_char = false;
            return 0; /* Avoid the menu beep for a handled Alt+digit. */
        }
        break;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (wparam == VK_MENU && window->alt_code) {
            /* TranslateMessage posts the Alt code's WM_CHAR before this key up is
             * dispatched, so it is already queued. */
            MSG typed;
            PeekMessageW(&typed, handle, WM_CHAR, WM_CHAR, PM_REMOVE);
            window->alt_code = false;
        }
        break;
    case WM_CHAR: {
        unsigned repeat = LOWORD(lparam);
        if (!repeat)
            repeat = 1;
        character(window, (wchar_t)wparam, repeat);
        changed(window);
        return 0;
    }
    case WM_UNICHAR:
        if (wparam == UNICODE_NOCHAR)
            return TRUE;
        codepoint(window, (uint32_t)wparam, 1);
        changed(window);
        return 0;
    case WM_TIMER:
        if (wparam == 1) {
            KillTimer(handle, 1);
            app_tick(window->app);
            changed(window);
        }
        return 0;
    case WM_SETFOCUS:
        session_focus(window->app, true);
        app_tick(window->app);
        changed(window);
        return 0;
    case WM_KILLFOCUS:
        session_focus(window->app, false);
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
    default:
        break;
    }
    return DefWindowProcW(handle, message, wparam, lparam);
}

static bool open_window(struct windows_window *window, struct app *app, int show)
{
    *window = (struct windows_window){.app = app, .dpi = 96};
    app->service = windows_service;
    app->service_data = window;
    /* Windows 10+; Wine may fall back to system DPI awareness. */
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        SetProcessDPIAware();
    HINSTANCE instance = GetModuleHandleW(NULL);
    WNDCLASSW klass = {0};
    klass.style = CS_DBLCLKS;
    klass.lpfnWndProc = window_proc;
    klass.hInstance = instance;
    klass.hCursor = LoadCursorW(NULL, IDC_IBEAM);
    klass.lpszClassName = window_class;
    if (!RegisterClassW(&klass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;
    UINT dpi = GetDpiForSystem();
    if (!dpi)
        dpi = 96;
    RECT size = {0, 0, MulDiv(app->width, (int)dpi, 96), MulDiv(app->height, (int)dpi, 96)};
    if (!AdjustWindowRectExForDpi(&size, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi))
        return false;
    bool maximized = app->maximized;
    HWND handle =
        CreateWindowExW(WS_EX_ACCEPTFILES, window_class, L"Jot",
                        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, size.right - size.left,
                        size.bottom - size.top, NULL, NULL, instance, window);
    if (!handle)
        return false;
    if (app->scratch) {
        wchar_t *title = windows_utf16(app->directory);
        if (title) {
            SetWindowTextW(handle, title);
            free(title);
        }
    }
    ShowWindow(handle, maximized ? SW_SHOWMAXIMIZED : show);
    UpdateWindow(handle);
    return window->handle != NULL;
}

int platform_run(struct app *app)
{
    struct windows_window window;
    if (!open_window(&window, app, SW_SHOWNORMAL)) {
        char error[512];
        windows_error(error, sizeof(error), "Create window", GetLastError());
        fprintf(stderr, "%s\n", error);
        return EXIT_FAILURE;
    }
    app_tick(app);
    changed(&window);
    MSG message;
    int result;
    while ((result = (int)GetMessageW(&message, NULL, 0, 0)) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (result < 0)
        app->failed = true;
    if (window.handle)
        DestroyWindow(window.handle);
    UnregisterClassW(window_class, GetModuleHandleW(NULL));
    return app->failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
