# Plan: split out a reusable window shell

Status: not started (written 2026-11-06).

## Goal

Separate mib into two parts:

- a **shell** that owns the window, input, clipboard and putting pixels on screen
- a **client** (mib) that draws frames and reacts to events.

When it's done, a second small tool should be able to link the same shell. Nothing should
change for mib users, and the binary should stay the same size (within about 1 KB).

Non-goals: widgets, layout, plugins, runtime backend selection, or a general GUI library.
The shell passes pixels, events and timers and does nothing more.

## Why

- Today `wayland.c` and `windows/window.c` include `app.h` and read and write mib fields
  directly: `dirty`, `title_hover`, `title_pressed`, `image_drag`, `drag_corner`,
  `context_menu`, `editor`, `service`. They also call mib-specific functions like
  `app_image_corner()` and `app_title_hit()`. Nothing in `app.h` marks which fields
  belong to the backends.
- Each backend turns raw keys into `enum editor_key` (Vim meanings such as `KEY_CTRL_O`
  and `KEY_PAGE`). That's why CLAUDE.md needs the rule "a new Ctrl key needs an entry
  in `src/editor.h` and in both platform files".
- `clipboard.inc` (`linux_service`) reads `app->editor.register_text`,
  `app->clipboard_image` and `session_path()`, so clipboard code depends on the editor.

A narrow boundary makes either side understandable without the other. That helps people
and LLMs, and it's what makes reuse possible.

## Design

Calls are resolved at link time, as `platform.h` already does. There is no table of function
pointers. The shell calls functions named `client_*`, each program defines them, and the
client calls `shell_*`.

```c
/* src/shell.h — the whole boundary. No OS headers, no app.h. */
struct shell_window {          /* owned by the shell, read by the client */
    int width, height;         /* logical pixels */
    uint32_t scale120;         /* physical per logical × 120 */
    bool maximized, tiled, decorated; /* decorated: the shell draws a title bar */
    int title_height;          /* physical pixels reserved at the top, 0 if none */
};
enum shell_keycode { SK_TEXT, SK_ESCAPE, SK_ENTER, SK_BACKSPACE, SK_DELETE, SK_TAB,
                     SK_LEFT, SK_RIGHT, SK_UP, SK_DOWN, SK_HOME, SK_END,
                     SK_PAGE_UP, SK_PAGE_DOWN, SK_CHAR /* letter/symbol with Ctrl/Alt */ };
enum { SHELL_CTRL = 1, SHELL_SHIFT = 2, SHELL_ALT = 4 };
struct shell_key {
    enum shell_keycode code;
    unsigned mods;
    uint32_t symbol;  /* lowercase codepoint for SK_CHAR, e.g. 'o' for Ctrl+O */
    char digit;       /* '0'..'9' for the number row by position or the keypad, else 0 */
    const char *text; /* committed UTF-8 for SK_TEXT */
};
enum shell_pointer_kind { SP_MOVE, SP_PRESS, SP_RELEASE, SP_SCROLL };
struct shell_pointer {
    enum shell_pointer_kind kind;
    int x, y, button;  /* logical pixels; button 1 left, 2 middle, 3 right */
    unsigned mods, clicks; /* clicks: 1, 2 for a double click (shell measures it) */
    int scroll;        /* rows, positive down */
};
enum shell_cursor { SC_DEFAULT, SC_TEXT, SC_POINTER, SC_RESIZE_NWSE, SC_RESIZE_NESW };

/* Client side: mib implements these. */
bool     client_paint(const struct shell_window *, uint32_t *px, int w, int h, int stride);
void     client_key(const struct shell_key *);
void     client_pointer(const struct shell_pointer *);
enum shell_cursor client_cursor(int x, int y);
void     client_tick(void);
uint64_t client_deadline(void);          /* ms on shell_now(), 0 for none */
void     client_close_requested(void);   /* may call shell_quit() */
void     client_paste(const char *text, size_t n, const char *image_path);
void     client_drop(const char *const *paths, unsigned count);
void     client_title_colors(uint32_t *bar, uint32_t *text, uint32_t *hover, uint32_t *close);

/* Shell side: each backend implements these. */
int      shell_run(const char *title, const char *app_id, int width, int height, bool maximized);
void     shell_redraw(void);              /* replaces writes to app->dirty */
void     shell_quit(bool failed);
void     shell_clipboard_text(const char *text, size_t n);
void     shell_clipboard_image(const char *png_path);
void     shell_clipboard_request(void);   /* answer comes back through client_paste */
void     shell_open(const char *path_or_url);
uint64_t shell_now(void);
```

Decisions behind it:

1. **The shell owns window decorations.** Title bar hit-testing, hover and pressed state,
   resize edges, double-click to maximize and the window menu move into the shell. Wayland
   needs the input serial for `xdg_toplevel_move`, `resize` and `show_window_menu`, so this
   can't sit cleanly on the client side. The shell draws the title bar after `client_paint`
   in the top `title_height` rows, using `client_title_colors`. It can use the existing
   `render_rectangle` and `render_rounded_box`, which move into a small shared
   `src/shell/draw.c`. Windows keeps its native frame (`decorated = false`).
2. **Keys reach the client as plain keys.** Backends report what was pressed and never
   what it means. A new shared `src/input.c` turns `struct shell_key` into
   `enum editor_key`: Ctrl+[ to Escape, Ctrl+Shift+C to copy image text, Ctrl+= to zoom,
   and Alt+`digit` to `KEY_PAGE`. After this, adding a Ctrl key touches `input.c` only.
   `digit` keeps today's layout-independent Alt+1..9 behaviour (evdev codes 2 to 11 and
   the keypad with Num Lock off, see `wayland.c` `handle_key`).
3. **Pointer events reach the client unprocessed, with a click count.** The shell counts
   double clicks (Windows can use `GetDoubleClickTime`). mib decides what Ctrl+click,
   double click, right click and image corner drags do. Backends stop checking
   `image_drag` and `context_menu`.
4. **The client chooses the cursor shape** with `client_cursor`. Resize edges stay in the
   shell. This replaces `app_image_corner` calls in the backends.
5. **Services are direct calls.** `app->service` and the `ED_COPY`, `ED_PASTE`, `ED_OPEN`
   and `ED_COPY_OCR` platform round trip are replaced by `shell_clipboard_*` and
   `shell_open`. The editor's `ED_*` request bits remain between the editor and the app.
   `app.c` turns them into shell calls. The clipboard code stops reading the editor.
6. **The 30 FPS frame cap and redraw merging stay in the shell.** `redraw_at`,
   `APP_FRAME_INTERVAL_MS`, `dirty`, `running` and `failed` all move there.

## Steps

Each step is one commit on `main`. Before each commit, `make test`, `make windows` and
`make vim-compare` must pass, and `make install` must run (see CLAUDE.md). Behaviour
doesn't change at any step.

0. **Baseline.** Record the stripped binary size (`ls -l ~/.local/bin/mib`, currently
   216,856 bytes) and the Windows `.exe` size. Run the manual checklist below once so
   there is something to compare against.
1. **Window state struct.** Add `src/shell.h` containing only `struct shell_window`. Embed
   it in `struct app` and move `width`, `height`, `scale120`, `maximized`, `tiled`,
   `client_decorated` and `title_height` into it. This is a mechanical rename.
2. **Key normalization (do this first if doing only one step).** Add `struct shell_key`,
   write `src/input.c` with mib's key bindings, and reduce `handle_key` in `wayland.c` and
   the `WM_KEYDOWN`/`WM_CHAR`/`WM_SYSKEYDOWN` handling in `window.c` to filling in a
   `shell_key`. Add a table-driven unit test for `input.c`. Remove the "both platform
   files" rule from CLAUDE.md.
3. **Pointer events.** Add `struct shell_pointer` and `client_pointer`. Move click counting
   into the backends. Move the Ctrl+click and double-click open, context menu and image
   drag logic out of `pointer_button` and the Windows button handlers into `app.c`.
4. **Cursor.** Add `client_cursor`. Map it to `wp_cursor_shape_device_v1` shapes on
   Wayland and to `SetCursor` on Windows. Remove `app_image_corner` calls from the
   backends.
5. **Decorations.** Move `title_hover`, `title_pressed`, `app_title_hit` and `title_bar()`
   from `render.c` and `app.c` into the shell, with the drawing helpers in
   `src/shell/draw.c`. The palette's `title_*` colours reach the shell through
   `client_title_colors`.
6. **Services.** Replace `app->service`/`service_data` with `shell_clipboard_*`,
   `shell_open` and `client_paste`/`client_drop`. Change `clipboard.inc` on both
   platforms so it no longer includes `app.h` or reads `app->editor`.
7. **Lifecycle and naming.** Rename `platform_run(app)` to `shell_run(...)`. Rename the
   remaining `app_*` functions the shell calls to `client_*`, implemented in a thin
   `src/client.c` that forwards to `app.c`. Move `dirty`, `redraw_at`, `running` and
   `failed` into the backends. **Done when** `grep -n 'app\.h\|editor\.h\|session\.h'
   src/platform/` returns nothing.
8. **Prove reuse.** Add `examples/hello/` with about 100 lines: a client that shows typed
   text in the bundled font and copies it with Ctrl+C. Build it in the Makefile and CMake
   for both platforms. If it needs anything from `app.c`, the boundary is wrong.
9. **Docs.** Update `docs/platforms.md`, the CLAUDE.md layout section and README.md.
   Delete this plan or mark it as done.

Optional later work: make the mib-specific editor hooks optional (`image_resize`,
`page_target`, `other_modified`, `ED_PAGE_*`, `ED_HELP`), so `editor.c`, `keys.c`, `ex.c`,
`history.c` and `substitute.c` can be reused as a Vim text engine on their own.

## Tests that need attention

- `tests/client-test.c` includes `src/platform/linux/wayland.c` directly and runs a fake
  `wayland-server`. `tests/windows-window-test.c` does the same with `window.c`. Both
  will need a stub client (a small set of `client_*` functions that record calls) in
  place of the real app. This is also the cleanest way to test the shell on its own.
- `tests/clipboard-test.c` and `tests/windows-clipboard-test.c` exercise the service path
  that step 6 replaces.
- `tests/modal-test.c`, `tests/app-test.c` and `make vim-compare` should pass unchanged.
  If they need edits, behaviour leaked across the boundary.

## Manual checklist (before step 1 and after steps 2 to 7)

On GNOME/Mutter (client-side decorations), a compositor with server-side decorations,
and Windows/Wine:

- Typing, key repeat, Ctrl+[ as Escape, Ctrl+O/Ctrl+I, zoom with Ctrl+= / Ctrl+- / Ctrl+0
- Alt+1..9 and Alt+0 on a non-US layout (e.g. French AZERTY) and on the keypad with
  Num Lock off
- Title bar drag, double-click to maximize, the min/max/close buttons and hover states,
  right-click window menu, resizing from edges and corners, rounded corners when
  floating and square when tiled or maximized
- Click to place the cursor, drag to select, scroll wheel
- Ctrl+click and double-click on an image to open it, right-click context menu, image
  corner resize drag
- Copy and paste text, paste an image, drag a file into the window, Ctrl+Shift+C OCR copy
- Closing with unsaved changes on another page is still refused

## Risks

- **Wayland serials.** Move, resize, the window menu and clipboard selection need the
  serial of the input event that triggered them. Keeping decorations and the clipboard
  in the shell handles this. Don't let the client trigger these actions directly.
- **Key repeat on Windows** goes through `WM_CHAR` and the repeat count. Check that
  `input.c` gets the same events as today, especially AltGr text (Ctrl+Alt on Windows),
  which must stay text and not become Ctrl shortcuts.
- **Scope creep.** Each new `shell_*` or `client_*` function should replace a field the
  backends currently reach into. If the header grows beyond about 30 declarations, look
  again before going on.
