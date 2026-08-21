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
    if (ok) {
        app->editor.on_save = app_saved;
        app->editor.on_save_data = app;
        media_sync(app);
    }
    return ok;
}

void app_saved(void *context)
{
    media_saved(context);
    session_save_positions(context);
}

void app_close(struct app *app)
{
    media_close(app);
    session_close(app);
    app_pages_close(app);
    tiny_font_close(app->font);
    editor_destroy(&app->editor);
    *app = (struct app){0};
}

void app_input(struct app *app, enum editor_key key, const char *text)
{
    media_sync(app);
    app->ocr_retry = false;
    app->image_hit = false;
    uint64_t search_serial = app->editor.search_serial;
    bool digit = key == KEY_TEXT && text && text[0] >= '1' && text[0] <= '9' && !text[1];
    if (app->page_map) {
        /* The map takes the keyboard: a digit (with or without Alt) shows that page, Up/Down
         * or k/j move the selection and Enter shows it, Esc closes the map, and other keys
         * are ignored rather than editing unseen text. Pages run 1 to 9 down the list. */
        unsigned choice = app->map_choice;
        char letter = key == KEY_TEXT && text && text[0] && !text[1] ? text[0] : 0;
        if ((key == KEY_UP || letter == 'k') && choice > 0)
            --choice;
        else if ((key == KEY_DOWN || letter == 'j') && choice < PAGE_COUNT - 1)
            ++choice;
        app->map_choice = choice;
        if ((key == KEY_PAGE || digit) && text && text[0] >= '1' && text[0] <= '9')
            app_page_switch(app, (unsigned)(text[0] - '1'));
        else if (key == KEY_ENTER)
            app_page_switch(app, choice);
        if (key == KEY_PAGE || digit || key == KEY_ESCAPE || key == KEY_ENTER)
            app->page_map = false;
        app->dirty = true;
        return;
    }
    if (key == KEY_PAGE) {
        /* Only from normal mode with nothing pending, so a switch never splits a command. */
        struct editor *ed = &app->editor;
        if (ed->mode == NORMAL && !ed->command_active && !ed->pending[0] && text &&
            text[0] >= '1' && text[0] <= '9') {
            app_page_switch(app, (unsigned)(text[0] - '1'));
            app->context_menu = false;
        }
        app->dirty = true;
        return;
    }
    if (app->context_menu) {
        if (key == KEY_ESCAPE) {
            app->context_menu = false;
            app->dirty = true;
            return;
        }
        if (key == KEY_UP || key == KEY_DOWN) {
            app->context_choice = (app->context_choice + (key == KEY_UP ? CONTEXT_ITEMS - 1 : 1)) %
                                  CONTEXT_ITEMS;
            app->dirty = true;
            return;
        }
        app->context_menu = false;
        if (key == KEY_ENTER) {
            if (app->context_choice == 0) {
                /* Copy Image: yank the image line, as y does on a selected image. */
                key = KEY_COPY;
                text = "";
            } else {
                if (app->service)
                    app->service(app, app->context_choice == 1 ? ED_COPY_OCR : ED_OPEN,
                                 app->service_data);
                app->dirty = true;
                return;
            }
        }
    }
    const struct media_entry *selected_image = app->editor.render_mode == RENDER_RAW ? NULL
                                               : media_at(app, app->editor.cursor);
    /* Images are atomic blocks in normal rendering. Do not join a neighboring
     * text line into the source reference when deleting across its boundary. */
    if (!selected_image && app->editor.render_mode == RENDER_NORMAL &&
        app->editor.mode == INSERT && !app->editor.command_active) {
        size_t cursor = app->editor.cursor;
        if (key == KEY_BACKSPACE && cursor &&
            app->editor.text[cursor - 1] == '\n')
            selected_image = media_at(app, cursor - 1);
        else if (key == KEY_DELETE && cursor < app->editor.length &&
                 app->editor.text[cursor] == '\n')
            selected_image = media_at(app, cursor + 1);
    }
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
    app->editor.extra_search = app->editor.render_mode == RENDER_RAW ? NULL : media_search;
    app->editor.extra_search_data = app;
    /* Set on every key: opening a page replaces the editor and its hooks. */
    app->editor.image_resize = media_resize;
    app->editor.image_resize_data = app;
    bool insert = app->editor.mode == INSERT;
    bool block_view = app->editor.render_mode == RENDER_NORMAL;
    bool block_delete_key = key == KEY_DELETE ||
        (block_view && insert && (key == KEY_BACKSPACE || key == KEY_CTRL_W || key == KEY_CTRL_U)) ||
        (app->editor.mode == NORMAL && key == KEY_TEXT && text &&
         (!strcmp(text, "d") || (block_view && !strcmp(text, "x"))));
    bool delete_image = selected_image && app->editor.render_mode != RENDER_POLISH &&
                        !app->editor.command_active &&
                        (app->editor.mode == NORMAL || insert) &&
                        !app->editor.pending[0] && block_delete_key;
    /* y copies a selected image at once, as yy does, ready to paste into other apps. */
    if (selected_image && app->editor.mode == NORMAL && !app->editor.command_active &&
        !app->editor.pending[0] && key == KEY_TEXT && text && !strcmp(text, "y"))
        key = KEY_COPY;
    bool image_insert = selected_image && app->editor.render_mode == RENDER_NORMAL &&
                        !app->editor.command_active && !app->editor.pending[0];
    if (image_insert && app->editor.mode == NORMAL && key == KEY_TEXT && text) {
        if (!strcmp(text, "i") || !strcmp(text, "I"))
            text = "O";
        else if (!strcmp(text, "a") || !strcmp(text, "A"))
            text = "o";
    }
    bool image_enter = image_insert && insert && key == KEY_ENTER;
    if (image_insert && insert && !delete_image &&
        ((key == KEY_TEXT && text && *text) || key == KEY_TAB || image_enter)) {
        if (!media_prepare_insert(app)) {
            app->dirty = true;
            return;
        }
    }
    if (delete_image) {
        /* Reuse line deletion, including its register and clipboard behavior. */
        size_t cursor = app->editor.cursor, start = selected_image->start;
        size_t before = app->editor.length;
        editor_change_end(&app->editor);
        app->editor.cursor = start;
        editor_input(&app->editor, KEY_CUT, "");
        editor_change_end(&app->editor);
        if (insert) {
            size_t removed = before - app->editor.length;
            app->editor.cursor = cursor < start ? cursor :
                                 cursor >= start + removed ? cursor - removed : start;
            if (app->editor.cursor > app->editor.length)
                app->editor.cursor = app->editor.length;
            app->editor.mode = INSERT;
            editor_change_begin(&app->editor);
        }
    } else if (!image_enter) {
        app->editor.other_modified = app_page_modified(app);
        editor_input(&app->editor, key, text);
    }
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
    if (requests & ED_PAGE_MAP) {
        if (app->root) {
            app->page_map = true;
            app->map_choice = app->current;
        }
        else
            snprintf(app->editor.message, sizeof(app->editor.message),
                     "Pages 1 to 9 need the notes folder, not a single file");
    }
    app->editor.requests &= ~(ED_ZOOM_IN | ED_ZOOM_OUT | ED_ZOOM_RESET | ED_PAGE_MAP);
    if ((app->editor.requests & ED_COPY) && image_name[0] && app->editor.register_text &&
        !strcmp(app->editor.register_text, image_markup))
        if ((app->clipboard_image = session_path(app, image_name)))
            app_toast(app, "Image copied");
    unsigned service_requests = app->editor.requests;
    app->editor.requests = 0;
    if (app->service)
        app->service(app, service_requests, app->service_data);
    else if (service_requests & ED_PASTE) {
        if (image_insert)
            media_paste_text(app, app->editor.register_text, app->editor.register_length);
        else
            editor_put(&app->editor, app->editor.register_text, app->editor.register_length,
                       app->editor.register_linewise, app->editor.put_after, app->editor.put_count);
    }
    free(app->clipboard_image);
    app->clipboard_image = NULL;
    app->follow_cursor = key != KEY_CTRL_E && key != KEY_CTRL_Y;
    app->dirty = true;
    if (app->editor.search_serial != search_serial) {
        const struct media_entry *hit = media_at(app, app->editor.cursor);
        app->image_hit = !app->editor.search_failed && hit && app->editor.match_start == hit->start;
        app->ocr_retry = app->editor.search_failed && app->editor.search_pending;
    }
    app_tick(app);
    if (app->editor.quit)
        app->running = false;
}

void app_toast(struct app *app, const char *text)
{
    snprintf(app->toast, sizeof(app->toast), "%s", text);
    app->toast_until = session_now() + APP_TOAST_MS;
    app->dirty = true;
}

void app_tick(struct app *app)
{
    media_tick(app);
    if (app->toast_until && session_now() >= app->toast_until) {
        app->toast[0] = 0;
        app->toast_until = 0;
        app->dirty = true;
    }
}

uint64_t app_deadline(const struct app *app)
{
    uint64_t work = app->work_at, toast = app->toast_until;
    return !work ? toast : !toast || work < toast ? work : toast;
}

void app_request_close(struct app *app)
{
    unsigned page = app_page_modified(app);
    if (app->editor.modified)
        snprintf(app->editor.message, sizeof(app->editor.message),
                 "Unsaved changes (:w to save, :q! to discard)");
    else if (page)
        snprintf(app->editor.message, sizeof(app->editor.message),
                 "Unsaved changes in page %u (Alt+%u to visit, :q! to discard)", page, page);
    else
        app->running = false;
    app->dirty = true;
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
int app_image_corner(const struct app *app, int x, int y)
{
    if (!app->image_box || app->editor.render_mode != RENDER_NORMAL)
        return -1;
    int64_t scale = app->scale120 ? app->scale120 : 120;
    int px = (int)(x * scale / 120), py = (int)(y * scale / 120);
    int reach = (int)(8 * scale / 120);
    for (int corner = 0; corner < 4; ++corner) {
        int cx = app->image_box_x + ((corner & 1) ? app->image_box_w : 0);
        int cy = app->image_box_y + ((corner & 2) ? app->image_box_h : 0);
        if (abs(px - cx) <= reach && abs(py - cy) <= reach)
            return corner;
    }
    return -1;
}
/* Corner drags keep the aspect ratio, following whichever direction moved further. */
static void drag_image(struct app *app, int x, int y)
{
    int64_t scale = app->scale120 ? app->scale120 : 120;
    int dx = (int)(x * scale / 120) - app->drag_x, dy = (int)(y * scale / 120) - app->drag_y;
    if (!(app->drag_corner & 1))
        dx = -dx;
    if (!(app->drag_corner & 2))
        dy = -dy;
    int across = app->drag_origin_height
                     ? (int)((int64_t)dy * app->drag_origin_width / app->drag_origin_height)
                     : 0;
    int width = app->drag_origin_width + (abs(dx) >= abs(across) ? dx : across);
    int least = (int)(16 * scale / 120);
    if (app->text_width > 0 && width > app->text_width)
        width = app->text_width;
    if (width < least)
        width = least;
    app->drag_width = width;
    app->dirty = true;
}
void app_pointer_release(struct app *app)
{
    app->map_click = false;
    if (!app->image_drag)
        return;
    app->image_drag = false;
    app->dirty = true;
    if (app->drag_width == app->drag_origin_width)
        return;
    int64_t scale = app->scale120 ? app->scale120 : 120;
    char spec[16];
    snprintf(spec, sizeof(spec), "%d", (int)((app->drag_width * 120 + scale / 2) / scale));
    media_resize(&app->editor, app->image_box_start, spec, app);
}
void app_pointer(struct app *app, int x, int y, bool extend)
{
    app->ocr_retry = false;
    app->image_hit = false;
    struct editor *ed = &app->editor;
    if (app->map_click && extend)
        return; /* The rest of the click that closed the map. */
    app->map_click = false;
    if (app->page_map) {
        /* A click on a page shows it; anywhere else closes the map. */
        int px = (int)((int64_t)x * app->scale120 / 120),
            py = (int)((int64_t)y * app->scale120 / 120);
        int page = extend ? -1 : navigator_page_at(app, px, py);
        if (page >= 0)
            app_page_switch(app, (unsigned)page);
        app->page_map = false;
        app->map_click = !extend;
        app->dirty = true;
        return;
    }
    if (app->image_drag) {
        if (extend)
            drag_image(app, x, y);
        return;
    }
    int corner = extend || app->context_menu ? -1 : app_image_corner(app, x, y);
    const struct media_entry *image = corner >= 0 ? media_at(app, app->image_box_start) : NULL;
    if (image && image == media_at(app, ed->cursor) && !ed->command_active) {
        int64_t scale = app->scale120 ? app->scale120 : 120;
        editor_change_end(ed);
        ed->mode = NORMAL;
        app->image_drag = true;
        app->drag_corner = corner;
        app->drag_x = (int)(x * scale / 120);
        app->drag_y = (int)(y * scale / 120);
        app->drag_width = app->drag_origin_width = app->image_box_w;
        app->drag_origin_height = app->image_box_h;
        app->dirty = true;
        app->follow_cursor = false;
        return;
    }
    if (app->context_menu && !extend) {
        int px = (int)((int64_t)x * app->scale120 / 120),
            py = (int)((int64_t)y * app->scale120 / 120);
        int width = 26 * app->cell;
        app->context_menu = false;
        app->dirty = true;
        if (px >= app->context_x && px < app->context_x + width && py >= app->context_y &&
            py < app->context_y + CONTEXT_ITEMS * app->line_height) {
            int item = (py - app->context_y) / app->line_height;
            if (item == 0)
                app_input(app, KEY_COPY, "");
            else if (app->service)
                app->service(app, item == 1 ? ED_COPY_OCR : ED_OPEN, app->service_data);
        }
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
    int width = 26 * app->cell, height = CONTEXT_ITEMS * app->line_height;
    if (px + width > app->view_width)
        px = app->view_width - width;
    int bottom =
        (int)((int64_t)app->height * app->scale120 / 120) - app->line_height - 2 * app->padding;
    if (py + height > bottom)
        py = bottom - height;
    app->context_x = px < 0 ? 0 : px;
    app->context_y = py < app->title_height ? app->title_height : py;
    app->context_choice = 0;
    app->context_menu = true;
    app->dirty = true;
}
