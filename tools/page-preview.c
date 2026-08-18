#include "app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv)
{
    if (argc < 3)
        return 2;
    struct app app;
    if (!app_open(&app, argv[1]))
        return 1;
    if (argc > 3)
        app.font_size = (unsigned)atoi(argv[3]);
    if (argc > 4)
        app.scale120 = (unsigned)atoi(argv[4]);
    for (int i = 5; i < argc; ++i) {
        if (!strcmp(argv[i], "light") || !strcmp(argv[i], "dark"))
            app.editor.light = !strcmp(argv[i], "light");
        else if (!strcmp(argv[i], "decorated"))
            app.client_decorated = true;
        else if (!strcmp(argv[i], "raw") || !strcmp(argv[i], "normal") || !strcmp(argv[i], "polish")) {
            char command[32];
            snprintf(command, sizeof(command), "set %s", argv[i]);
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
