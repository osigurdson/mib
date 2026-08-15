#define _POSIX_C_SOURCE 200809L
#include "app.h"
#include "media.h"
#include "platform.h"
#include "session.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include "platform/windows/win32.h"
#else
#include <unistd.h>
#endif
int main(int argc, char **argv)
{
    char directory[1024];
#ifdef _WIN32
    SetEnvironmentVariableA("TINYVI_OCR", "0");
    _putenv("TINYVI_OCR=0");
    char temp[MAX_PATH];
    assert(GetTempPathA(MAX_PATH, temp));
    snprintf(directory, sizeof(directory), "%stinyvi-media-%lu-%lu", temp,
             (unsigned long)GetCurrentProcessId(), (unsigned long)GetTickCount());
#else
    setenv("TINYVI_OCR", "0", 1);
    snprintf(directory, sizeof(directory), "/tmp/tinyvi-media-XXXXXX");
    assert(mkdtemp(directory));
#endif
    unsigned char rgba[320 * 180 * 4];
    for (unsigned y = 0; y < 180; ++y)
        for (unsigned x = 0; x < 320; ++x) {
            size_t p = (y * 320 + x) * 4;
            rgba[p] = (unsigned char)(x * 255 / 320);
            rgba[p + 1] = (unsigned char)(y * 255 / 180);
            rgba[p + 2] = 100;
            rgba[p + 3] = 255;
        }
    size_t bytes;
    unsigned char *png = image_png(rgba, 320, 180, &bytes);
    assert(png);
    int w, h;
    assert(image_info(png, bytes, &w, &h) && w == 320 && h == 180);
    struct image image = {0};
    assert(image_decode(&image, png, bytes));
    assert(!memcmp(image.rgba, rgba, sizeof(rgba)));
    image_free(&image);
    assert(!image_decode(&image, png, 12));
    struct app app;
    assert(app_open(&app, NULL));
    assert(session_open(&app, directory) == 1);
    assert(!app.media);
    assert(media_paste_image(&app, png, bytes));
    free(png);
    assert(strstr(app.editor.text, "![](assets/"));
    const struct media_entry *entry = media_at(&app, app.editor.cursor);
    assert(entry && entry->width == 320 && entry->height == 180);
    char relative[272], name[256];
    snprintf(name, sizeof(name), "%s", entry->name);
    snprintf(relative, sizeof(relative), "%s.txt", name);
    char *path = session_path(&app, relative);
    assert(session_write_new(path, "Hello screenshot OCR\n", 21));
    free(path);
    snprintf(relative, sizeof(relative), "%s.tsv", name);
    path = session_path(&app, relative);
    const char *tsv =
        "level\tpage_num\tblock_num\tpar_num\tline_num\tword_"
        "num\tleft\ttop\twidth\theight\tconf\ttext\n5\t1\t1\t1\t1\t1\t10\t10\t100\t30\t90\tHello\n";
    assert(session_write_new(path, tsv, strlen(tsv)));
    free(path);
    app_input(&app, KEY_TEXT, "G");
    app_input(&app, KEY_TEXT, "/");
    app_input(&app, KEY_TEXT, "screenshot");
    app_input(&app, KEY_ENTER, "");
    assert(media_at(&app, app.editor.cursor));
    const uint32_t scales[] = {120, 150, 180, 240};
    const unsigned sizes[] = {12, 16, 18, 22};
    for (unsigned i = 0; i < 4; ++i) {
        app.scale120 = scales[i];
        app.font_size = sizes[i];
        app.editor.top = 0;
        uint32_t *pixels = malloc(800 * 560 * 4);
        assert(pixels);
        assert(app_paint(&app, pixels, 800, 560, 800));
        assert(app_view_rows(&app, 0) > 1);
        assert(app_hit_test(&app, 100 * 120 / scales[i], 60 * 120 / scales[i]) == 0);
        if (argc > 1) {
            char output[1024];
            snprintf(output, sizeof(output), "%s-%u.ppm", argv[1], i);
            FILE *f = fopen(output, "wb");
            assert(f);
            fprintf(f, "P6\n800 560\n255\n");
            for (int p = 0; p < 800 * 560; ++p) {
                unsigned char rgb[] = {pixels[p] >> 16, pixels[p] >> 8, pixels[p]};
                fwrite(rgb, 1, 3, f);
            }
            fclose(f);
        }
        free(pixels);
    }
    char *target = media_target(&app);
    assert(target && strstr(target, name));
    free(target);
    size_t n;
    char *ocr = media_ocr_text(&app, &n);
    assert(ocr && strstr(ocr, "screenshot"));
    free(ocr);
    app_input(&app, KEY_TEXT, "d");
    app_input(&app, KEY_TEXT, "d");
    assert(!strstr(app.editor.text, "![]"));
    app_input(&app, KEY_TEXT, "u");
    assert(strstr(app.editor.text, "![]"));
    app_close(&app);
    const char *files[] = {"jot.md",        "AGENTS.md", "CLAUDE.md", ".tinyvi.lock",
                           ".tinyvi-state", name,        NULL};
    for (unsigned i = 0; files[i]; ++i) {
        snprintf(relative, sizeof(relative), "%s", files[i]);
        char full[1400];
        snprintf(full, sizeof(full), "%s/%s", directory, relative);
#ifdef _WIN32
        wchar_t *w = windows_path(full);
        DeleteFileW(w);
        free(w);
#else
        unlink(full);
#endif
    }
    for (unsigned i = 0; i < 2; ++i) {
        char full[1400];
        snprintf(full, sizeof(full), "%s/%s.%s", directory, name, i ? "tsv" : "txt");
#ifdef _WIN32
        wchar_t *w = windows_path(full);
        DeleteFileW(w);
        free(w);
#else
        unlink(full);
#endif
    }
    char assets[1200];
    snprintf(assets, sizeof(assets), "%s/assets", directory);
#ifdef _WIN32
    wchar_t *wa = windows_path(assets), *wd = windows_path(directory);
    RemoveDirectoryW(wa);
    RemoveDirectoryW(wd);
    free(wa);
    free(wd);
#else
    rmdir(assets);
    rmdir(directory);
#endif
    puts("Bounded image codec, assets, inline rendering/scales, OCR search and undo passed");
}
