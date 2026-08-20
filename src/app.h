#ifndef APP_H
#define APP_H
#include "editor.h"
#include "tiny-font.h"
#include "markdown.h"

/* Round up to whole milliseconds so redraws never exceed 30 FPS. */
enum { APP_FRAME_INTERVAL_MS = (1000 + 30 - 1) / 30 };

enum app_title_action { TITLE_NONE, TITLE_MOVE, TITLE_MINIMIZE, TITLE_MAXIMIZE, TITLE_CLOSE };

/* Settings shared by every page and file, kept in the user's config folder. */
struct app_settings {
    int width, height;
    unsigned font_size;
    enum render_mode render_mode;
    enum color_scheme colorscheme;
    bool maximized, light;
};

/* Pages have nine rooms laid out like a numeric keypad (7 8 9 / 4 5 6 / 1 2 3). Each room is
 * a folder with its own jot.md and assets/: room 1 is the page folder itself and rooms 2 to 9
 * are its room2/ to room9/ subfolders. The shown room lives in app.editor, app.media and
 * app.directory; the others keep their document and media here once loaded, while registers,
 * search and settings stay with app.editor. */
enum { ROOM_COUNT = 9 };
struct room {
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
    int owner_fd, activation_fd;
    void *owner_handle;
    uint64_t page_id, work_at, redraw_at;
    void (*service)(struct app *, unsigned, void *);
    void *service_data;
    int width, height;
    int restore_width, restore_height; /* Logical window dimensions. */
    uint32_t scale120;                 /* Physical pixels per logical pixel × 120. */
    bool dirty, running, failed, follow_cursor;
    /* The settings as loaded, so closing writes back only what this window changed. */
    bool settings_loaded;
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
    /* The shown room (0 for room 1), the others, and the :map overlay with its selected
     * room, first cell (room 7) and cell size from the last paint, in physical pixels. */
    unsigned room, map_choice;
    char *page; /* The page folder, which is room 1's; NULL for explicit files. */
    struct room rooms[ROOM_COUNT];
    bool room_map, map_click;
    int map_x, map_y, map_width, map_height, map_gap;
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
/* Show room index (0 for room 1). Returns false, with a message, when there are no rooms. */
bool app_room_switch(struct app *app, unsigned index);
/* The room's first H1 heading text, or "" when it has none. */
void app_room_heading(struct app *app, unsigned index, char *heading, size_t size);
/* The first other room with unsaved changes (1 to 9), or 0. */
unsigned app_room_modified(const struct app *app);
void app_rooms_close(struct app *app);
int app_main(int argc, char **argv);
bool app_open(struct app *app, const char *filename);
void app_close(struct app *app);
void app_input(struct app *app, enum editor_key key, const char *text);
void app_request_close(struct app *app);
/* Top-down premultiplied ARGB8888 pixels; stride is in uint32_t pixels, not bytes. */
bool app_paint(struct app *app, uint32_t *pixels, int width, int height, int stride);
#endif
