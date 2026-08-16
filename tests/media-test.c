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
    const char *tsv = "level\tpage_num\tblock_num\tpar_num\tline_num\tword_"
                      "num\tleft\ttop\twidth\theight\tconf\ttext\n5\t1\t1\t1\t1\t1\t10\t10\t100\t30"
                      "\t90\tHello\n5\t1\t1\t1\t1\t2\t120\t10\t100\t30\t90\tscreenshot\n";
    assert(session_write_new(path, tsv, strlen(tsv)));
    free(path);
    app_input(&app, KEY_TEXT, "G");
    app_input(&app, KEY_TEXT, "/");
    app_input(&app, KEY_TEXT, "Hello screenshot");
    app_input(&app, KEY_ENTER, "");
    assert(media_at(&app, app.editor.cursor));
    assert(app.image_hit);
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
        int image_x = app.padding + app.gutter, image_y = app.padding + 2;
        assert(pixels[(size_t)(image_y - 3) * 800 + image_x - 3] == 0xffe5edf5);
        assert(pixels[(size_t)(image_y - 3) * 800 + image_x + 10] == 0xff88c0d0);
        int px = app.padding + app.gutter + 115, py = app.padding + 2 + 20;
        uint32_t raw = 0xff000000u | ((115u * 255 / 320) << 16) | ((20u * 255 / 180) << 8) | 100;
        assert(pixels[(size_t)py * 800 + px] != raw); /* phrase spans the gap between words */
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
    /* Both delete keys remove the entire image and preserve the input mode. */
    for (unsigned mode = 0; mode < 2; ++mode)
        for (unsigned del = 0; del < 2; ++del) {
            app.editor.cursor = 0;
            app.editor.mode = mode ? INSERT : NORMAL;
            enum editor_mode expected_mode = app.editor.mode;
            app_input(&app, del ? KEY_DELETE : KEY_TEXT, del ? "" : "d");
            assert(!strstr(app.editor.text, "![]"));
            assert(app.editor.mode == expected_mode);
            assert(app.editor.register_linewise && strstr(app.editor.register_text, "![]"));
            editor_undo(&app.editor, false);
            assert(strstr(app.editor.text, "![]"));
        }
    app.editor.mode = NORMAL;
    /* A command's d remains command text even while an image is selected. */
    app.editor.cursor = 0;
    app_input(&app, KEY_TEXT, ":");
    app_input(&app, KEY_TEXT, "d");
    assert(strstr(app.editor.text, "![]") && !strcmp(app.editor.command, "d"));
    app_input(&app, KEY_ESCAPE, "");
    /* Mixed notes retain their images when copied into a different page. */
    assert(editor_replace(&app.editor, 0, 0, "before\n", 7));
    assert(editor_replace(&app.editor, app.editor.length, app.editor.length, "after\n", 6));
    /* Deleting an image between notes leaves both neighboring lines intact. */
    char *image_ref = strstr(app.editor.text, "![](");
    assert(image_ref);
    app.editor.cursor = (size_t)(image_ref - app.editor.text);
    app_input(&app, KEY_DELETE, "");
    assert(!strcmp(app.editor.text, "before\nafter\n"));
    app_input(&app, KEY_TEXT, "u");
    assert(strstr(app.editor.text, "![]("));
    app.editor.cursor = 0;
    app_input(&app, KEY_TEXT, "V");
    app_input(&app, KEY_TEXT, "G");
    app_input(&app, KEY_TEXT, "y");
    size_t fragment_length;
    char *fragment = media_fragment(&app, &fragment_length);
    assert(fragment);
    struct app other;
    assert(app_open(&other, NULL));
    other.directory = session_path(&app, "other");
    assert(other.directory && session_mkdir(other.directory));
    assert(media_paste_fragment(&other, (unsigned char *)fragment, fragment_length));
    free(fragment);
    assert(strstr(other.editor.text, "before\n") && strstr(other.editor.text, "after\n"));
    char *ref = strstr(other.editor.text, "![](");
    assert(ref);
    other.editor.cursor = (size_t)(ref - other.editor.text);
    const struct media_entry *copied = media_at(&other, other.editor.cursor);
    assert(copied && copied->width == 320 && copied->height == 180);
    char *copied_path = media_target(&other), *other_assets = session_path(&other, "assets");
    assert(copied_path && other_assets);
    char *other_directory = malloc(strlen(other.directory) + 1);
    assert(other_directory);
    strcpy(other_directory, other.directory);
    app_close(&other);
#ifdef _WIN32
    wchar_t *wc = windows_path(copied_path), *wa2 = windows_path(other_assets),
            *wd2 = windows_path(other_directory);
    assert(DeleteFileW(wc));
    assert(RemoveDirectoryW(wa2));
    assert(RemoveDirectoryW(wd2));
    free(wc);
    free(wa2);
    free(wd2);
#else
    assert(!unlink(copied_path));
    assert(!rmdir(other_assets));
    assert(!rmdir(other_directory));
#endif
    free(copied_path);
    free(other_assets);
    free(other_directory);
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
