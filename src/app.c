#include "app.h"
#include "session.h"
#include <stdio.h>
#include <string.h>

bool app_open(struct app *app, const char *filename)
{
    *app = (struct app){.width = 800,
                        .height = 560,
                        .scale120 = 120,
                        .owner_fd = -1,
                        .activation_fd = -1,
                        .font_size = 18,
                        .follow_cursor = true,
                        .dirty = true,
                        .running = true};
    return editor_open(&app->editor, filename);
}

void app_close(struct app *app)
{
    session_close(app);
    tiny_font_close(app->font);
    editor_destroy(&app->editor);
    *app = (struct app){0};
}

void app_input(struct app *app, enum editor_key key, const char *text)
{
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
    unsigned service_requests = app->editor.requests;
    app->editor.requests = 0;
    if (app->service)
        app->service(app, service_requests, app->service_data);
    else if (service_requests & ED_PASTE)
        editor_put(&app->editor, app->editor.register_text, app->editor.register_length,
                   app->editor.register_linewise, app->editor.put_after, app->editor.put_count);
    if (app->editor.revision != before)
        session_changed(app);
    app->follow_cursor = key != KEY_CTRL_E && key != KEY_CTRL_Y;
    app->dirty = true;
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
            ed->top = app_row_next(ed, ed->top, ed->viewport_columns ? ed->viewport_columns : 80);
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
    struct editor *ed = &app->editor;
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
