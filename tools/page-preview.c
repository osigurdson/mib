#include "app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv)
{
    if (argc < 3)
        return 2;
    /* Render argv[1] as a page, with its images relative to its folder. */
    struct app app;
    if (!app_open(&app))
        return 1;
    editor_destroy(&app.editor);
    app.editor = (struct editor){0};
    const char *slash = strrchr(argv[1], '/');
    size_t n = slash ? (size_t)(slash - argv[1]) : 1;
    app.directory = malloc(n + 1);
    if (!app.directory || !editor_open(&app.editor, argv[1]))
        return 1;
    memcpy(app.directory, slash ? argv[1] : ".", n);
    app.directory[n] = 0;
    app.media_scanned = false; /* the new document has the same revision */
    if (argc > 3)
        app.font_size = (unsigned)atoi(argv[3]);
    if (argc > 4)
        app.scale120 = (unsigned)atoi(argv[4]);
    for (int i = 5; i < argc; ++i) {
        if (!strcmp(argv[i], "light") || !strcmp(argv[i], "dark"))
            app.editor.light = !strcmp(argv[i], "light");
        else if (!strcmp(argv[i], "decorated"))
            app.client_decorated = true;
        else if (!strcmp(argv[i], "source") || !strcmp(argv[i], "normal") ||
                 !strcmp(argv[i], "reading")) {
            char command[32];
            snprintf(command, sizeof(command), "set view=%s", argv[i]);
            editor_command(&app.editor, command);
        } else if (!editor_colorscheme(&app.editor, argv[i]))
            return 2;
    }
    int width = (int)(800 * app.scale120 / 120), height = (int)(560 * app.scale120 / 120);
    uint32_t *pixels = malloc((size_t)width * height * 4);
    if (!pixels)
        return 1;
    if (!app_paint(&app, pixels, width, height, width))
        return 1;
    FILE *f = fopen(argv[2], "wb");
    if (!f)
        return 1;
    fprintf(f, "P6\n%d %d\n255\n", width, height);
    for (int i = 0; i < width * height; ++i) {
        unsigned char rgb[] = {pixels[i] >> 16, pixels[i] >> 8, pixels[i]};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    free(pixels);
    app_close(&app);
    return 0;
}
