#include "app.h"
#include "edit-internal.h"
#include "media.h"
#include "session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The editor fields that belong to a room's document. Mode, registers, search, dot repeat,
 * command history and settings stay with app.editor across switches. */
#define ROOM_FIELDS                                                                                \
    X(text) X(filename) X(length) X(capacity) X(cursor) X(top) X(left) X(anchor)                   \
    X(desired_column) X(visual_first) X(visual_last) X(modified) X(crlf) X(revision)               \
    X(saved_hash) X(history) X(context) X(jump_count) X(jump_index)

static void stash(struct editor *ed, struct room *room)
{
#define X(field) room->field = ed->field;
    ROOM_FIELDS
#undef X
    memcpy(room->marks, ed->marks, sizeof(room->marks));
    memcpy(room->jumps, ed->jumps, sizeof(room->jumps));
    room->loaded = true;
}
static void unstash(struct editor *ed, struct room *room)
{
#define X(field) ed->field = room->field;
    ROOM_FIELDS
#undef X
    memcpy(ed->marks, room->marks, sizeof(ed->marks));
    memcpy(ed->jumps, room->jumps, sizeof(ed->jumps));
    *room = (struct room){0};
}
static void free_room(struct room *room)
{
    struct editor document = {0};
    media_free(room->media);
    unstash(&document, room);
    editor_destroy(&document);
}
/* A room's folder, or with name, a file in it: the page folder for room 1, room2/ to room9/
 * inside it for the others. */
static char *room_path(const struct app *app, unsigned index, const char *name)
{
    char folder[16] = "";
    if (index)
        snprintf(folder, sizeof(folder), "/room%u", index + 1);
    size_t n = strlen(app->page) + strlen(folder) + (name ? strlen(name) + 1 : 0) + 1;
    char *path = malloc(n);
    if (path)
        snprintf(path, n, "%s%s%s%s", app->page, folder, name ? "/" : "", name ? name : "");
    return path;
}
/* Read a room's jot.md, which may not exist yet. New rooms open at the end, like the page
 * itself. Loading never creates the room's folder; showing it does. */
static bool load(struct app *app, unsigned index)
{
    struct room *room = &app->rooms[index];
    if (room->loaded)
        return true;
    char *path = room_path(app, index, "jot.md");
    struct editor document = {0};
    bool ok = path && editor_open(&document, path);
    free(path);
    if (ok) {
        document.cursor = document.length;
        stash(&document, room);
        room->fresh = true;
    } else {
        snprintf(app->editor.message, sizeof(app->editor.message), "Could not open room %u: %.200s",
                 index + 1, document.message[0] ? document.message : "out of memory");
        editor_destroy(&document);
    }
    return ok;
}
bool app_room_switch(struct app *app, unsigned index)
{
    struct editor *ed = &app->editor;
    if (!app->page) {
        snprintf(ed->message, sizeof(ed->message), "Rooms are only available in pages");
        return false;
    }
    if (index >= ROOM_COUNT)
        return false;
    if (index == app->room)
        return true;
    char *folder = room_path(app, index, NULL);
    if (!folder || !session_mkdir(folder)) {
        snprintf(ed->message, sizeof(ed->message), "Could not create room %u's folder", index + 1);
        free(folder);
        return false;
    }
    if (!load(app, index)) {
        free(folder);
        return false;
    }
    editor_change_end(ed);
    struct room *leaving = &app->rooms[app->room], *entering = &app->rooms[index];
    stash(ed, leaving);
    leaving->fresh = false;
    leaving->media = app->media;
    app->media = entering->media;
    bool fresh = entering->fresh;
    unstash(ed, entering);
    free(app->directory);
    app->directory = folder;
    app->room = index;
    ed->mode = NORMAL;
    ed->pending[0] = 0;
    ed->match_start = ed->match_end = 0;
    ed->search_hidden = true;
    ed->message[0] = 0;
    /* Positions cached for the previous room's text are no longer valid. */
    app->markdown.valid = false;
    app->media_scanned = false;
    app->image_box = app->image_drag = app->image_hit = false;
    app->context_menu = false;
    /* Keep the room's own scroll position; a room shown for the first time follows its
     * cursor to the end instead. */
    app->follow_cursor = fresh;
    /* Pick up changes made outside Jot while the room was hidden, as on focus, and resume
     * the room's image work. */
    session_focus(app, true);
    media_tick(app);
    app->dirty = true;
    return true;
}
void app_room_heading(struct app *app, unsigned index, char *heading, size_t size)
{
    heading[0] = 0;
    if (index >= ROOM_COUNT || !size)
        return;
    const char *text;
    size_t length;
    if (index == app->room) {
        text = app->editor.text;
        length = app->editor.length;
    } else {
        if (!app->page || !load(app, index))
            return;
        text = app->rooms[index].text;
        length = app->rooms[index].length;
    }
    char fenced = 0;
    for (size_t start = 0; start < length;) {
        const char *newline = memchr(text + start, '\n', length - start);
        size_t end = newline ? (size_t)(newline - text) : length;
        /* Fences as editor_fence finds them: up to three spaces, then ``` or ~~~. */
        size_t p = start;
        while (p < end && p - start < 3 && text[p] == ' ')
            ++p;
        char marker = p + 3 <= end && (text[p] == '`' || text[p] == '~') &&
                              text[p + 1] == text[p] && text[p + 2] == text[p]
                          ? text[p]
                          : 0;
        if (marker)
            fenced = fenced ? (fenced == marker ? 0 : fenced) : marker;
        else if (!fenced && end - start >= 2 && text[start] == '#' && text[start + 1] == ' ') {
            size_t from = start + 2, to = end;
            while (from < to && (text[from] == ' ' || text[from] == '\t'))
                ++from;
            while (to > from && (text[to - 1] == ' ' || text[to - 1] == '\t' ||
                                 text[to - 1] == '\r'))
                --to;
            /* An optional closing sequence: "# Title #". */
            size_t closing = to;
            while (closing > from && text[closing - 1] == '#')
                --closing;
            if (closing < to && (closing == from || text[closing - 1] == ' ')) {
                to = closing;
                while (to > from && text[to - 1] == ' ')
                    --to;
            }
            size_t n = to - from < size - 1 ? to - from : size - 1;
            while (n && n < to - from && ((unsigned char)text[from + n] & 0xc0) == 0x80)
                --n; /* Don't cut a UTF-8 sequence. */
            memcpy(heading, text + from, n);
            heading[n] = 0;
            return;
        }
        start = end + 1;
    }
}
unsigned app_room_modified(const struct app *app)
{
    for (unsigned i = 0; i < ROOM_COUNT; ++i)
        if (i != app->room && app->rooms[i].loaded && app->rooms[i].modified)
            return i + 1;
    return 0;
}
void app_rooms_close(struct app *app)
{
    for (unsigned i = 0; i < ROOM_COUNT; ++i)
        if (app->rooms[i].loaded)
            free_room(&app->rooms[i]);
    app->room = 0;
}
