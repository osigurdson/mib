#ifndef APP_H
#define APP_H
#include "editor.h"
#include "tiny-font.h"
#include "markdown.h"

/* Round up to whole milliseconds so redraws never exceed 30 FPS. */
enum { APP_FRAME_INTERVAL_MS = (1000 + 30 - 1) / 30 };

/* The image context menu: Copy Image, Copy Text from Image, Open Image. */
enum { CONTEXT_ITEMS = 3 };

enum app_title_action { TITLE_NONE, TITLE_MOVE, TITLE_MINIMIZE, TITLE_MAXIMIZE, TITLE_CLOSE };

/* Settings shared by every page and file, kept in the user's config folder. */
struct app_settings {
    int width, height;
    unsigned font_size;
    enum view_mode view;
    enum color_scheme colorscheme;
    bool maximized, light;
};

/* The notes folder holds nine pages, 1/ to 9/, each with its own jot.md and assets/, shown in
 * numeric keypad order on the map (7 8 9 / 4 5 6 / 1 2 3). The shown page lives in app.editor,
 * app.media and app.directory; the others keep their document and media here once loaded,
 * while registers, search and settings stay with app.editor. */
enum { PAGE_COUNT = 9 };
struct page {
    char *text, *filename;
    size_t length, capacity, cursor, top, left, anchor, desired_column;
    size_t visual_first, visual_last;
    bool loaded, fresh, modified, crlf;
    uint64_t revision, saved_hash;
    struct edit_history *history;
    struct media *media;
    size_t marks[26], context, jumps[100];
    unsigned jump_count, jump_index;
};

struct app {
    struct editor editor;
    struct md_cache markdown;
    struct tiny_font *font;
    struct media *media;
    char *directory, *clipboard_image;
    uint64_t media_revision;
    bool media_scanned, image_hit, ocr_retry;
    bool scratch, maximized, context_menu, client_decorated, tiled;
    int context_x, context_y, context_choice;
    enum app_title_action title_hover, title_pressed;
    uint64_t work_at, redraw_at;
    /* A short status bar notice from app_toast, shown until toast_until (session_now ms). */
    char toast[64];
    uint64_t toast_until;
    void (*service)(struct app *, unsigned, void *);
    void *service_data;
    int width, height;
    int restore_width, restore_height; /* Logical window dimensions. */
    uint32_t scale120;                 /* Physical pixels per logical pixel × 120. */
    bool dirty, running, failed, follow_cursor;
    /* The settings as loaded, so closing writes back only what this window changed. */
    bool settings_loaded;
    /* Set with the settings: saves and closing record cursor positions in the positions file. */
    bool remember_positions;
    struct app_settings settings;
    unsigned font_size;
    int cell, line_height, padding, gutter, view_width, title_height;
    int text_x, text_width, text_height;
    /* The selected image's box from the last paint, in physical pixels, and a corner drag
     * resizing it: drag_width previews the new width until the button is released. */
    bool image_box, image_drag;
    size_t image_box_start;
    int image_box_x, image_box_y, image_box_w, image_box_h;
    int drag_corner, drag_x, drag_y, drag_width, drag_origin_width, drag_origin_height;
    /* The shown page (0 for page 1), the others, and the page list overlay with its selected
     * page, first row (page 1) and row size from the last paint, in physical pixels. */
    unsigned current, map_choice;
    char *root; /* The notes folder holding 1/ to 9/; NULL for explicit files. */
    struct page pages[PAGE_COUNT];
    bool page_map, map_click;
    bool help; /* the :help card is showing */
    int map_x, map_y, map_width, map_height;
};
size_t app_view_next(struct app *app, size_t start);
int app_view_rows(struct app *app, size_t start);
/* Physical pixels occupied by the visual row beginning at this source offset. */
int app_view_height(struct app *app, size_t start);
size_t app_row_next(const struct editor *ed, size_t start, unsigned columns);
/* Rounded CSD radius in the requested pixel scale; zero for square windows. */
int app_corner_radius(const struct app *app, int width, int height, uint32_t scale120);
enum app_title_action app_title_hit(const struct app *app, int logical_x, int logical_y);
size_t app_hit_test(struct app *app, int logical_x, int logical_y);
void app_scroll(struct app *app, int rows);
void app_context(struct app *app, int x, int y);
void app_pointer(struct app *app, int x, int y, bool extend);
/* The left button was released; finishes an image resize drag. */
void app_pointer_release(struct app *app);
/* The selected image's corner handle (0 top left, 1 top right, 2 bottom left, 3 bottom
 * right) under a logical position, or -1. */
int app_image_corner(const struct app *app, int x, int y);
/* Show page index (0 for page 1). Returns false, with a message, for explicit files. */
bool app_page_switch(struct app *app, unsigned index);
/* The page's first H1 heading text, or "" when it has none. */
void app_page_heading(struct app *app, unsigned index, char *heading, size_t size);
/* The first other page with unsaved changes (1 to 9), or 0. */
unsigned app_page_modified(const struct app *app);
/* Save every other page with unsaved changes, as :wa does after saving the shown one.
 * False, with the reason in the message, if one could not be saved. */
bool app_pages_save(struct app *app);
void app_pages_close(struct app *app);
int app_main(int argc, char **argv);
bool app_open(struct app *app, const char *filename);
void app_close(struct app *app);
/* The editor's on_save hook: clean up unused assets and remember cursor positions. */
void app_saved(void *context);
void app_input(struct app *app, enum editor_key key, const char *text);
void app_request_close(struct app *app);
/* Show a brief notice in the status bar, such as "Image copied". Editor messages take
 * precedence, and the notice clears itself after APP_TOAST_MS. */
enum { APP_TOAST_MS = 1200 };
void app_toast(struct app *app, const char *text);
/* Background work for the event loop: media jobs and toast expiry. */
void app_tick(struct app *app);
/* When the event loop should next call app_tick (session_now ms), or 0 for never. */
uint64_t app_deadline(const struct app *app);
/* The listed page under physical pixel (x, y) from the last paint, or -1; see navigator.c. */
int navigator_page_at(const struct app *app, int x, int y);
/* Top-down premultiplied ARGB8888 pixels; stride is in uint32_t pixels, not bytes. */
bool app_paint(struct app *app, uint32_t *pixels, int width, int height, int stride);
#endif
