#ifndef RENDER_INTERNAL_H
#define RENDER_INTERNAL_H
#include "app.h"
/* Shared between render.c and the overlays in navigator.c and help.c. Pixels are premultiplied
 * ARGB8888 with stride in pixels; drawing is clipped to width x height. */
struct palette {
    uint32_t background, text, dim, link, heading, quote, done, italic;
    uint32_t code, search, selection, cursor, status, menu, menu_selected, handle;
    uint32_t title, title_separator, title_hover, title_pressed;
    uint32_t search_text, selection_text, status_text, menu_text, menu_selected_text;
};
uint32_t render_blend(uint32_t background, unsigned alpha, uint32_t foreground);
void render_rectangle(uint32_t *pixels, int width, int height, int stride, int x, int y, int w,
                      int h, uint32_t color);
/* A filled w x h box with corners of the given radius. */
void render_rounded_box(uint32_t *pixels, int width, int height, int stride, int x, int y, int w,
                        int h, int radius, uint32_t color);
/* Text at the current font size, one glyph per cell from x on the given baseline. */
bool render_label(struct app *app, uint32_t *pixels, int width, int height, int stride,
                  const char *text, int x, int baseline, int cell, uint32_t color);
/* The editor's font size for the app's zoom and scale. */
unsigned render_font_size(const struct app *app);
/* The page list overlay between rows top and bottom; see navigator.c. */
bool navigator_draw(struct app *app, const struct palette *theme, uint32_t *pixels, int width,
                    int height, int stride, int top, int bottom, int padding);
/* The :help card between rows top and bottom; see help.c. */
bool help_draw(struct app *app, const struct palette *theme, uint32_t *pixels, int width,
               int height, int stride, int top, int bottom, int padding);
#endif
