#ifndef APP_H
#define APP_H
#include "editor.h"
#include "tiny-font.h"

struct app {
    struct editor editor;
    struct tiny_font *font;
    int width, height;                 /* Logical window dimensions. */
    uint32_t scale120;                 /* Physical pixels per logical pixel × 120. */
    bool dirty, running, failed;
};
int app_main(int argc, char **argv);
bool app_open(struct app *app, const char *filename);
void app_close(struct app *app);
void app_input(struct app *app, enum editor_key key, const char *text);
void app_request_close(struct app *app);
/* Top-down 0xFFRRGGBB pixels; stride is in uint32_t pixels, not bytes. */
bool app_paint(struct app *app, uint32_t *pixels, int width, int height, int stride);
#endif
