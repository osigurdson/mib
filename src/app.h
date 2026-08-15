#ifndef APP_H
#define APP_H
#include "editor.h"
#include "tiny-font.h"

struct app {
    struct editor editor;
    struct tiny_font *font;
    struct media *media;
    char *directory, *clipboard_image;
    uint64_t media_revision;
    bool media_scanned;
    bool scratch, maximized, context_menu;
    int context_x, context_y, context_choice;
    int owner_fd, activation_fd;
    void *owner_handle;
    uint64_t page_id, save_at, work_at;
    void (*service)(struct app *, unsigned, void *);
    void *service_data;
    int width, height; /* Logical window dimensions. */
    uint32_t scale120; /* Physical pixels per logical pixel × 120. */
    bool dirty, running, failed, follow_cursor;
    unsigned font_size;
    int cell, line_height, padding, gutter, view_width;
};
size_t app_view_next(struct app *app, size_t start);
int app_view_rows(struct app *app, size_t start);
size_t app_row_next(const struct editor *ed, size_t start, unsigned columns);
size_t app_hit_test(struct app *app, int logical_x, int logical_y);
void app_scroll(struct app *app, int rows);
void app_context(struct app *app, int x, int y);
void app_pointer(struct app *app, int x, int y, bool extend);
int app_main(int argc, char **argv);
bool app_open(struct app *app, const char *filename);
void app_close(struct app *app);
void app_input(struct app *app, enum editor_key key, const char *text);
void app_request_close(struct app *app);
/* Top-down 0xFFRRGGBB pixels; stride is in uint32_t pixels, not bytes. */
bool app_paint(struct app *app, uint32_t *pixels, int width, int height, int stride);
#endif
