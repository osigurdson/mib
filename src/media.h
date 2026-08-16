#ifndef MEDIA_H
#define MEDIA_H
#include "editor.h"
#include "image.h"
struct app;
struct media_entry {
    size_t start, end;
    char name[256];
    int width, height;
    bool ocr_done;
};
void media_close(struct app *app);
void media_sync(struct app *app);
void media_tick(struct app *app);
const struct media_entry *media_at(struct app *app, size_t position);
int media_rows(struct app *app, size_t position, int available_width, int line_height,
               int max_rows);
bool media_pixels(struct app *app, const struct media_entry *entry, struct image *image);
char *media_clipboard_image(struct app *app, size_t *length);
char *media_target(struct app *app);
char *media_ocr_text(struct app *app, size_t *length);
bool media_paste_image(struct app *app, const unsigned char *data, size_t length);
char *media_fragment(struct app *app, size_t *length);
bool media_paste_fragment(struct app *app, const unsigned char *data, size_t length);
void media_paste_text(struct app *app, const char *text, size_t length);
void media_import_uris(struct app *app, const char *uris, size_t length);
void media_import_file(struct app *app, const char *path);
size_t media_search(const struct editor *ed, size_t origin, bool forward, void *context);
/* Highlight matching OCR words in an already displayed image. */
void media_highlight(struct app *app, const struct media_entry *entry, uint32_t *pixels, int width,
                     int height, int stride, int x, int y, int draw_width, int draw_height);
#endif
