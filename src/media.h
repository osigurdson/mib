#ifndef MEDIA_H
#define MEDIA_H
#include "editor.h"
#include "image.h"
struct app;
struct media_entry {
    size_t start, end;
    char name[256];
    int width, height;
    /* Width from Obsidian's "![alt|300](...)" size suffix in logical pixels, or 0. The
     * suffix (including its "|") runs from alt_size to the closing bracket at alt_end. */
    int size;
    size_t alt_size, alt_end;
    bool ocr_done;
};
struct media;
/* Stop image work on every page, then remove images this window pasted that no saved note
 * mentions. Call before the notes folder and pages are released. */
void media_close(struct app *app);
/* Stop a media state's OCR job and free it, as media_close does for app->media. */
void media_free(struct media *m);
void media_sync(struct app *app);
/* Successful-save notification; removes tracked, unreferenced assets. */
void media_saved(void *context);
void media_tick(struct app *app);
const struct media_entry *media_at(struct app *app, size_t position);
int media_rows(struct app *app, size_t position, int available_width, int line_height,
               int max_rows);
/* Physical size an image is drawn at, fitted to available_width when that is positive. */
void media_display_size(struct app *app, const struct media_entry *entry, int available_width,
                        int *width, int *height);
/* The editor's image_resize hook; see struct editor. */
bool media_resize(struct editor *ed, size_t position, const char *spec, void *data);
bool media_pixels(struct app *app, const struct media_entry *entry, struct image *image);
char *media_clipboard_image(struct app *app, size_t *length);
/* The path or URL of the image or link under the cursor, or NULL. */
char *media_target(struct app *app);
/* media_target, for the desktop to open: NULL with a message instead when the image isn't a
 * PNG or JPEG named as one, or a link names a file that opening could run (a program, script,
 * shortcut, installer or desktop launcher). http, https and mailto links always pass. */
char *media_open_target(struct app *app);
char *media_ocr_text(struct app *app, size_t *length);
bool media_paste_image(struct app *app, const unsigned char *data, size_t length);
char *media_fragment(struct app *app, size_t *length);
bool media_paste_fragment(struct app *app, const unsigned char *data, size_t length);
/* Move insert-mode typing onto a text line when the cursor is on an image. */
bool media_prepare_insert(struct app *app);
void media_paste_text(struct app *app, const char *text, size_t length);
/* Paste the files a text/uri-list names, or one file: an image as an image, and a text file's
 * text only when TEXT is set. Leave TEXT unset when the list may not come from the user's own
 * choice of files, such as any app's clipboard, so a link to a private file can't have its
 * contents pasted into a note; such a file isn't even read. */
void media_import_uris(struct app *app, const char *uris, size_t length, bool text);
void media_import_file(struct app *app, const char *path, bool text);
size_t media_search(const struct editor *ed, size_t origin, bool forward, void *context);
/* Highlight matching OCR words in an already displayed image. */
void media_highlight(struct app *app, const struct media_entry *entry, uint32_t *pixels, int width,
                     int height, int stride, int x, int y, int draw_width, int draw_height);
#endif
