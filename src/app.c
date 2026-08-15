#include "app.h"
#include <stdio.h>

bool app_open(struct app *app, const char *filename)
{
    *app = (struct app){.width = 800, .height = 560, .scale120 = 120,
                        .dirty = true, .running = true};
    return editor_open(&app->editor, filename);
}

void app_close(struct app *app)
{
    tiny_font_close(app->font);
    editor_destroy(&app->editor);
    *app = (struct app){0};
}

void app_input(struct app *app, enum editor_key key, const char *text)
{
    editor_input(&app->editor, key, text);
    app->dirty = true;
    if (app->editor.quit) app->running = false;
}

void app_request_close(struct app *app)
{
    if (app->editor.modified) {
        snprintf(app->editor.message, sizeof(app->editor.message),
                 "Unsaved changes (:w to save, :q! to discard)");
        app->dirty = true;
    } else app->running = false;
}
