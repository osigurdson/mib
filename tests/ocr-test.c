#define _POSIX_C_SOURCE 200809L
#include "app.h"
#include "media.h"
#include "session.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
int main(void)
{
    unsetenv("MIB_OCR");
    char directory[] = "/tmp/mib-ocr-XXXXXX";
    assert(mkdtemp(directory));
    struct app app;
    assert(app_open(&app));
    app.directory = strdup(directory);
    assert(app.directory);
    struct tiny_font *font = tiny_font_open();
    assert(font && tiny_font_set_size(font, 40 * 64));
    struct tiny_font_metrics metrics = tiny_font_metrics(font);
    unsigned char *rgba = malloc(640 * 100 * 4);
    assert(rgba);
    memset(rgba, 255, 640 * 100 * 4);
    const char *text = "HELLO WORLD";
    int x = 20;
    for (size_t i = 0; text[i]; ++i) {
        const struct tiny_glyph *g = tiny_font_glyph(font, (unsigned char)text[i]);
        assert(g);
        for (int row = 0; row < g->height; ++row)
            for (int col = 0; col < g->width; ++col) {
                int px = x + g->left + col, py = 65 - g->top + row;
                if (px < 0 || py < 0 || px >= 640 || py >= 100)
                    continue;
                unsigned char alpha = g->alpha[row * g->width + col];
                size_t at = ((size_t)py * 640 + px) * 4;
                rgba[at] = rgba[at + 1] = rgba[at + 2] = (unsigned char)(255 - alpha);
            }
        x += metrics.advance;
    }
    tiny_font_close(font);
    size_t n;
    unsigned char *png = image_png(rgba, 640, 100, &n);
    free(rgba);
    assert(png);
    assert(media_paste_image(&app, png, n));
    free(png);
    assert(app.work_at);
    uint64_t deadline = session_now() + 15000;
    while (app.work_at && session_now() < deadline) {
        struct timespec delay = {.tv_nsec = 100000000};
        nanosleep(&delay, NULL);
        media_tick(&app);
    }
    assert(!app.work_at);
    char *ocr = media_ocr_text(&app, &n);
    if (!ocr)
        fprintf(stderr, "Tesseract did not produce text\n");
    assert(ocr);
    if (!strstr(ocr, "HELLO WORLD"))
        fprintf(stderr, "OCR text: %s\n", ocr);
    assert(strstr(ocr, "HELLO WORLD"));
    free(ocr);
    char *asset = media_target(&app);
    assert(asset);
    char sidecar[2048];
    snprintf(sidecar, sizeof(sidecar), "%s.tsv", asset);
    struct stat permissions;
    assert(!stat(sidecar, &permissions) && (permissions.st_mode & 0777) == 0600);
    char *tsv = session_read(sidecar, 2 * 1024 * 1024, &n);
    assert(tsv && strstr(tsv, "HELLO"));
    free(tsv);
    app_input(&app, KEY_TEXT, "/");
    app_input(&app, KEY_TEXT, "WORLD");
    app_input(&app, KEY_ENTER, "");
    assert(media_at(&app, app.editor.cursor));

    /* Deterministic delayed child for retry/cancellation: do not depend on a
       machine being slow enough to type before the real OCR engine finishes. */
    char bin[2048], script[2200];
    snprintf(bin, sizeof(bin), "%s/bin", directory);
    assert(!mkdir(bin, 0700));
    snprintf(script, sizeof(script), "%s/tesseract", bin);
    const char *program =
        "#!/bin/sh\nsleep 0.25\nprintf '%s\\n' \"$MIB_TEST_WORD\" > \"$2.txt\"\nprintf "
        "'level\\tpage_num\\tblock_num\\tpar_num\\tline_num\\tword_"
        "num\\tleft\\ttop\\twidth\\theight\\tconf\\ttext\\n5\\t1\\t1\\t1\\t1\\t1\\t20\\t20\\t100\\t"
        "30\\t90\\t%s\\n' \"$MIB_TEST_WORD\" > \"$2.tsv\"\n";
    FILE *wrapper = fopen(script, "wb");
    assert(wrapper);
    assert(fwrite(program, 1, strlen(program), wrapper) == strlen(program));
    assert(!fclose(wrapper));
    assert(!chmod(script, 0700));
    const char *old_path = getenv("PATH");
    char *saved_path = strdup(old_path ? old_path : "");
    assert(saved_path);
    size_t path_length = strlen(bin) + strlen(saved_path) + 2;
    char *new_path = malloc(path_length);
    assert(new_path);
    snprintf(new_path, path_length, "%s:%s", bin, saved_path);
    assert(!setenv("PATH", new_path, 1));
    free(new_path);
    size_t encoded_length;
    char *encoded = session_read(asset, 32 * 1024 * 1024, &encoded_length);
    assert(encoded);
    char *extra_assets[2] = {0};
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        editor_change_end(&app.editor);
        assert(editor_replace(&app.editor, 0, app.editor.length, "top\nbottom\n", 11));
        app.editor.mode = NORMAL;
        app.editor.cursor = 0;
        assert(!setenv("MIB_TEST_WORD", attempt ? "WOMBAT" : "NUMBAT", 1));
        assert(media_paste_image(&app, (unsigned char *)encoded, encoded_length));
        extra_assets[attempt] = media_target(&app);
        assert(extra_assets[attempt] && app.work_at);
        app_input(&app, KEY_TEXT, "/");
        app_input(&app, KEY_TEXT, attempt ? "wombat" : "numbat");
        assert(app.ocr_retry && app.editor.search_failed);
        if (!attempt) {
            app_input(&app, KEY_ENTER, "");
            app_input(&app, KEY_TEXT, "g");
            app_input(&app, KEY_TEXT, "g");
            assert(!app.ocr_retry && app.editor.cursor == 0);
        }
        deadline = session_now() + 5000;
        while (app.work_at && session_now() < deadline) {
            struct timespec delay = {.tv_nsec = 10000000};
            nanosleep(&delay, NULL);
            media_tick(&app);
        }
        assert(!app.work_at);
        if (attempt) {
            assert(app.editor.command_active && app.image_hit && media_at(&app, app.editor.cursor));
            app_input(&app, KEY_ESCAPE, "");
        } else
            assert(app.editor.cursor == 0 && !app.image_hit);
    }
    /* Saving a deletion stops an in-flight OCR child before removing sidecars. */
    app.editor.mode = NORMAL;
    assert(media_paste_image(&app, (unsigned char *)encoded, encoded_length));
    char *removed_asset = media_target(&app);
    assert(removed_asset && app.work_at);
    assert(editor_replace(&app.editor, 0, app.editor.length, "", 0));
    char *note_path = session_path(&app, "page.md");
    assert(note_path && editor_save(&app.editor, note_path));
    struct timespec settle = {.tv_nsec = 350000000};
    nanosleep(&settle, NULL);
    for (unsigned i = 0; i < 3; ++i) {
        char check[2400];
        snprintf(check, sizeof(check), "%s%s", removed_asset,
                 i == 0   ? ""
                 : i == 1 ? ".txt"
                          : ".tsv");
        assert(access(check, F_OK) != 0);
    }
    free(removed_asset);
    free(encoded);
    assert(!setenv("PATH", saved_path, 1));
    free(saved_path);
    unsetenv("MIB_TEST_WORD");
    unlink(script);
    rmdir(bin);
    app_close(&app);
    assert(!unlink(note_path));
    free(note_path);
    snprintf(sidecar, sizeof(sidecar), "%s.txt", asset);
    unlink(sidecar);
    snprintf(sidecar, sizeof(sidecar), "%s.tsv", asset);
    unlink(sidecar);
    unlink(asset);
    free(asset);
    for (unsigned i = 0; i < 2; ++i) {
        snprintf(sidecar, sizeof(sidecar), "%s.txt", extra_assets[i]);
        unlink(sidecar);
        snprintf(sidecar, sizeof(sidecar), "%s.tsv", extra_assets[i]);
        unlink(sidecar);
        unlink(extra_assets[i]);
        free(extra_assets[i]);
    }
    snprintf(sidecar, sizeof(sidecar), "%s/assets", directory);
    rmdir(sidecar);
    rmdir(directory);
    puts("Actual Tesseract child: bundled-font image, text/TSV sidecars and OCR search passed");
}
