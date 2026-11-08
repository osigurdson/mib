/* Drive the real editor from a script on stdin and write each frame it draws, for the README
 * demo (tools/demo.py). Opens the notes folder in $MIB_DIR like mib itself.
 *
 *   demo-frames OUTPUT_DIR SCALE120
 *
 * Commands, one per line:
 *   type TEXT    type TEXT a character at a time, one frame per character
 *   key NAME     press esc or enter
 *   page N       Alt+N: show page N, or the page list for 0
 *   paste FILE   press p with the PNG in FILE on the clipboard
 *   hold MS      show the last frame for MS milliseconds
 *   speed MS     how long each typed character stays on screen (default 70)
 *   begin        start the recording over from the current screen, after setup keys
 *
 * Writes OUTPUT_DIR/NNNN.ppm per frame and prints "NNNN MS" lines: the frame and how long it
 * stays on screen, and "begin" where the recording starts over. */
#include "app.h"
#include "media.h"
#include "session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct app app;
static uint32_t *pixels;
static int width, height, frames;
static unsigned typing = 70;
static const char *folder;

static void frame(unsigned ms)
{
    if (!app_paint(&app, pixels, width, height, width)) {
        fprintf(stderr, "paint failed\n");
        exit(1);
    }
    char path[1100];
    snprintf(path, sizeof(path), "%s/%04d.ppm", folder, frames);
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        exit(1);
    }
    fprintf(f, "P6\n%d %d\n255\n", width, height);
    for (int i = 0; i < width * height; ++i) {
        unsigned char rgb[] = {pixels[i] >> 16, pixels[i] >> 8, pixels[i]};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
    printf("%04d %u\n", frames++, ms);
}
/* The image on the pretend clipboard, handed over when the editor asks to paste, as the
 * platform backends do. */
static unsigned char clipboard[1 << 22];
static size_t clipboard_length;
static void service(struct app *a, unsigned requests, void *data)
{
    (void)data;
    if ((requests & ED_PASTE) && clipboard_length)
        media_paste_image(a, clipboard, clipboard_length);
}
static void paste(const char *file)
{
    FILE *f = fopen(file, "rb");
    if (!f) {
        perror(file);
        exit(1);
    }
    clipboard_length = fread(clipboard, 1, sizeof(clipboard), f);
    fclose(f);
    app_input(&app, KEY_TEXT, "p");
}
int main(int argc, char **argv)
{
    if (argc != 3)
        return 2;
    folder = argv[1];
    if (!app_open(&app) || session_open(&app, NULL) < 0)
        return 1;
    app.scale120 = (unsigned)atoi(argv[2]);
    app.client_decorated = true;
    app.service = service;
    width = (int)(800 * app.scale120 / 120);
    height = (int)(560 * app.scale120 / 120);
    pixels = malloc((size_t)width * height * 4);
    if (!pixels)
        return 1;
    frame(0);
    char line[1024];
    while (fgets(line, sizeof(line), stdin)) {
        line[strcspn(line, "\n")] = 0;
        char *argument = strchr(line, ' ');
        argument = argument ? argument + 1 : line + strlen(line);
        if (!strncmp(line, "type ", 5))
            for (const char *c = argument; *c; ++c) {
                char text[2] = {*c, 0};
                app_input(&app, KEY_TEXT, text);
                frame(typing);
            }
        else if (!strncmp(line, "key ", 4)) {
            app_input(&app, !strcmp(argument, "esc") ? KEY_ESCAPE : KEY_ENTER, "");
            frame(typing);
        } else if (!strncmp(line, "page ", 5)) {
            app_input(&app, KEY_PAGE, argument);
            frame(typing);
        } else if (!strncmp(line, "paste ", 6)) {
            paste(argument);
            frame(typing);
        } else if (!strncmp(line, "hold ", 5))
            printf("hold %s\n", argument);
        else if (!strcmp(line, "begin")) {
            frames = 0;
            printf("begin\n");
            frame(0);
        } else if (!strncmp(line, "speed ", 6))
            typing = (unsigned)atoi(argument);
        else if (line[0] && line[0] != '#') {
            fprintf(stderr, "Unknown demo command: %s\n", line);
            return 2;
        }
    }
    app_close(&app);
    free(pixels);
    return 0;
}
