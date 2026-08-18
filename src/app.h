#ifndef APP_H
#define APP_H
#include "editor.h"
#include "tiny-font.h"
#include "markdown.h"

/* Round up to whole milliseconds so redraws never exceed 30 FPS. */
enum { APP_FRAME_INTERVAL_MS = (1000 + 30 - 1) / 30 };

enum app_title_action { TITLE_NONE, TITLE_MOVE, TITLE_MINIMIZE, TITLE_MAXIMIZE, TITLE_CLOSE };

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
    unsigned font_size;
    int cell, line_height, padding, gutter, view_width, title_height;
    int text_x, text_width, text_height;
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
int app_main(int argc, char **argv);
bool app_open(struct app *app, const char *filename);
void app_close(struct app *app);
void app_input(struct app *app, enum editor_key key, const char *text);
void app_request_close(struct app *app);
/* Top-down premultiplied ARGB8888 pixels; stride is in uint32_t pixels, not bytes. */
bool app_paint(struct app *app, uint32_t *pixels, int width, int height, int stride);
#endif
