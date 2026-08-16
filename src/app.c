#include "app.h"
#include "media.h"
#include "session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool app_open(struct app *app, const char *filename)
{
    *app = (struct app){.width = 800,
                        .height = 560,
                        .restore_width = 800,
                        .restore_height = 560,
                        .scale120 = 120,
                        .owner_fd = -1,
                        .activation_fd = -1,
                        .font_size = 18,
                        .follow_cursor = true,
                        .dirty = true,
                        .running = true};
    bool ok = editor_open(&app->editor, filename);
    if (ok && filename) {
        const char *slash = strrchr(filename, '/'), *back = strrchr(filename, '\\');
        if (back && (!slash || back > slash))
            slash = back;
        size_t n = slash ? (size_t)(slash - filename) : 1;
        if (slash == filename || (slash && slash > filename && slash[-1] == ':'))
            ++n;
        app->directory = malloc(n + 1);
        if (!app->directory)
            return false;
        if (slash)
            memcpy(app->directory, filename, n);
        else
            app->directory[0] = '.';
        app->directory[n] = 0;
    }
    return ok;
}

void app_close(struct app *app)
{
    media_close(app);
    session_close(app);
    tiny_font_close(app->font);
    editor_destroy(&app->editor);
    *app = (struct app){0};
}

void app_input(struct app *app, enum editor_key key, const char *text)
{
    app->ocr_retry = false;
    app->image_hit = false;
    uint64_t search_serial = app->editor.search_serial;
    if (app->context_menu) {
        if (key == KEY_ESCAPE) {
            app->context_menu = false;
            app->dirty = true;
            return;
        }
        if (key == KEY_UP || key == KEY_DOWN) {
            app->context_choice ^= 1;
            app->dirty = true;
            return;
        }
        if (key == KEY_ENTER) {
            unsigned request = app->context_choice ? ED_OPEN : ED_COPY_OCR;
            app->context_menu = false;
            if (app->service)
                app->service(app, request, app->service_data);
            app->dirty = true;
            return;
        }
        app->context_menu = false;
    }
    if (app->scratch &&
        ((key == KEY_CTRL_Q) ||
         (key == KEY_ENTER && app->editor.command_active && app->editor.command_kind == ':' &&
          (!strcmp(app->editor.command, "q") || !strcmp(app->editor.command, "w") ||
           !strcmp(app->editor.command, "wq") || !strcmp(app->editor.command, "x"))))) {
        bool quit = key == KEY_CTRL_Q || strcmp(app->editor.command, "w");
        app->editor.command_active = false;
        if (session_save(app) && quit) {
            app->editor.quit = true;
            app->running = false;
        }
        app->dirty = true;
        return;
    }
    const struct media_entry *selected_image = media_at(app, app->editor.cursor);
    char image_name[256] = {0}, image_markup[280] = {0};
    if (selected_image) {
        size_t n = selected_image->end - selected_image->start;
        if (n < sizeof(image_markup) - 2) {
            memcpy(image_markup, app->editor.text + selected_image->start, n);
            image_markup[n] = '\n';
        }
    }
    if (selected_image)
        snprintf(image_name, sizeof(image_name), "%s", selected_image->name);
    app->editor.extra_search = media_search;
    app->editor.extra_search_data = app;
    uint64_t before = app->editor.revision;
    editor_input(&app->editor, key, text);
    if (app->editor.revision != before)
        session_changed(app);
    unsigned requests = app->editor.requests;
    if (requests & ED_ZOOM_IN) {
        if (app->font_size < 48)
            ++app->font_size;
    }
    if (requests & ED_ZOOM_OUT) {
        if (app->font_size > 8)
            --app->font_size;
    }
    if (requests & ED_ZOOM_RESET)
        app->font_size = 18;
    app->editor.requests &= ~(ED_ZOOM_IN | ED_ZOOM_OUT | ED_ZOOM_RESET);
    if ((app->editor.requests & ED_COPY) && image_name[0] && app->editor.register_text &&
        !strcmp(app->editor.register_text, image_markup))
        app->clipboard_image = session_path(app, image_name);
    unsigned service_requests = app->editor.requests;
    app->editor.requests = 0;
    if (app->service)
        app->service(app, service_requests, app->service_data);
    else if (service_requests & ED_PASTE)
        editor_put(&app->editor, app->editor.register_text, app->editor.register_length,
                   app->editor.register_linewise, app->editor.put_after, app->editor.put_count);
    free(app->clipboard_image);
    app->clipboard_image = NULL;
    if (app->editor.revision != before)
        session_changed(app);
    app->follow_cursor = key != KEY_CTRL_E && key != KEY_CTRL_Y;
    app->dirty = true;
    if (app->editor.search_serial != search_serial) {
        const struct media_entry *hit = media_at(app, app->editor.cursor);
        app->image_hit = !app->editor.search_failed && hit && app->editor.match_start == hit->start;
        app->ocr_retry = app->editor.search_failed && app->editor.search_pending;
    }
    media_tick(app);
    if (app->editor.quit)
        app->running = false;
}

void app_request_close(struct app *app)
{
    if (app->scratch && app->editor.modified && session_save(app))
        app->running = false;
    else if (app->editor.modified) {
        snprintf(app->editor.message, sizeof(app->editor.message),
                 "Unsaved changes (:w to save, :q! to discard)");
        app->dirty = true;
    } else
        app->running = false;
}

void app_scroll(struct app *app, int rows)
{
    struct editor *ed = &app->editor;
    if (rows > 0)
        while (rows--)
            ed->top = app_view_next(app, ed->top);
    else
        while (rows++) {
            if (!ed->top)
                break;
            size_t start = editor_line_start(ed, ed->top);
            ed->top = start < ed->top ? start : editor_line_start(ed, start - 1);
        }
    app->follow_cursor = false;
    app->dirty = true;
}
void app_pointer(struct app *app, int x, int y, bool extend)
{
    app->ocr_retry = false;
    app->image_hit = false;
    struct editor *ed = &app->editor;
    if (app->context_menu && !extend) {
        int px = (int)((int64_t)x * app->scale120 / 120),
            py = (int)((int64_t)y * app->scale120 / 120);
        int width = 26 * app->cell;
        if (px >= app->context_x && px < app->context_x + width && py >= app->context_y &&
            py < app->context_y + 2 * app->line_height) {
            unsigned request = py < app->context_y + app->line_height ? ED_COPY_OCR : ED_OPEN;
            if (app->service)
                app->service(app, request, app->service_data);
        }
        app->context_menu = false;
        app->dirty = true;
        return;
    }
    if (extend) {
        if (ed->mode != VISUAL) {
            ed->anchor = ed->cursor;
            ed->mode = VISUAL;
        }
    } else {
        editor_change_end(ed);
        ed->mode = NORMAL;
    }
    ed->cursor = app_hit_test(app, x, y);
    app->dirty = true;
    app->follow_cursor = false;
}

void app_context(struct app *app, int x, int y)
{
    app_pointer(app, x, y, false);
    if (!media_at(app, app->editor.cursor))
        return;
    int px = (int)((int64_t)x * app->scale120 / 120), py = (int)((int64_t)y * app->scale120 / 120);
    int width = 26 * app->cell, height = 2 * app->line_height;
    if (px + width > app->view_width)
        px = app->view_width - width;
    int bottom =
        (int)((int64_t)app->height * app->scale120 / 120) - 2 * app->line_height - app->padding;
    if (py + height > bottom)
        py = bottom - height;
    app->context_x = px < 0 ? 0 : px;
    app->context_y = py < 0 ? 0 : py;
    app->context_choice = 0;
    app->context_menu = true;
    app->dirty = true;
}
