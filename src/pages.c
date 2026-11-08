#include "app.h"
#include "edit-internal.h"
#include "media.h"
#include "session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The editor fields that belong to a page's document. Mode, registers, search, dot repeat,
 * command history and settings stay with app.editor across switches. */
/* clang-format off */
#define ROOM_FIELDS                                                                                \
    X(text) X(filename) X(length) X(capacity) X(cursor) X(top) X(left) X(anchor)                   \
    X(desired_column) X(visual_first) X(visual_last) X(modified) X(crlf) X(revision)               \
    X(saved_hash) X(history) X(context) X(jump_count) X(jump_index)
/* clang-format on */

static void stash(struct editor *ed, struct page *page)
{
#define X(field) page->field = ed->field;
    ROOM_FIELDS
#undef X
    memcpy(page->marks, ed->marks, sizeof(page->marks));
    memcpy(page->jumps, ed->jumps, sizeof(page->jumps));
    page->loaded = true;
}
static void unstash(struct editor *ed, struct page *page)
{
#define X(field) ed->field = page->field;
    ROOM_FIELDS
#undef X
    memcpy(ed->marks, page->marks, sizeof(ed->marks));
    memcpy(ed->jumps, page->jumps, sizeof(ed->jumps));
    *page = (struct page){0};
}
static void free_page(struct page *page)
{
    struct editor document = {0};
    media_free(page->media);
    unstash(&document, page);
    editor_destroy(&document);
}
/* A page's folder, 1/ to 9/ in the notes folder, or with name, a file in it. */
static char *page_path(const struct app *app, unsigned index, const char *name)
{
    char folder[16];
    snprintf(folder, sizeof(folder), "/%u", index + 1);
    size_t n = strlen(app->root) + strlen(folder) + (name ? strlen(name) + 1 : 0) + 1;
    char *path = malloc(n);
    if (path)
        snprintf(path, n, "%s%s%s%s", app->root, folder, name ? "/" : "", name ? name : "");
    return path;
}
/* Read a page's page.md, which may not exist yet, with the cursor where it was last left, or at
 * the end for a page with no remembered position. Loading never creates the page's folder;
 * showing it does. */
static bool load(struct app *app, unsigned index)
{
    struct page *page = &app->pages[index];
    if (page->loaded)
        return true;
    char *path = page_path(app, index, "page.md");
    struct editor document = {0};
    bool ok = path && editor_open(&document, path);
    free(path);
    if (ok) {
        document.cursor = document.length;
        if (app->remember_positions)
            session_restore_position(&document);
        stash(&document, page);
        page->fresh = true;
    } else {
        snprintf(app->editor.message, sizeof(app->editor.message), "Could not open page %u: %.200s",
                 index + 1, document.message[0] ? document.message : "out of memory");
        editor_destroy(&document);
    }
    return ok;
}
bool app_page_switch(struct app *app, unsigned index)
{
    struct editor *ed = &app->editor;
    if (!app->root)
        return false;
    if (index >= PAGE_COUNT)
        return false;
    if (index == app->current)
        return true;
    char *folder = page_path(app, index, NULL);
    if (!folder || !session_mkdir(folder)) {
        snprintf(ed->message, sizeof(ed->message), "Could not create page %u's folder", index + 1);
        free(folder);
        return false;
    }
    if (!load(app, index)) {
        free(folder);
        return false;
    }
    editor_change_end(ed);
    struct page *leaving = &app->pages[app->current], *entering = &app->pages[index];
    stash(ed, leaving);
    leaving->fresh = false;
    leaving->media = app->media;
    app->media = entering->media;
    bool fresh = entering->fresh;
    unstash(ed, entering);
    free(app->directory);
    app->directory = folder;
    app->current = index;
    ed->mode = NORMAL;
    ed->pending[0] = 0;
    ed->match_start = ed->match_end = 0;
    ed->search_hidden = true;
    ed->message[0] = 0;
    /* Positions cached for the previous page's text are no longer valid. */
    app->markdown.valid = false;
    app->media_scanned = false;
    app->image_box = app->image_drag = app->image_hit = false;
    app->context_menu = false;
    /* Keep the page's own scroll position; a page shown for the first time follows its
     * cursor to the end instead. */
    app->follow_cursor = fresh;
    /* Pick up changes made outside mib while the page was hidden, as on focus, and resume
     * the page's image work. */
    session_focus(app, true);
    media_tick(app);
    app->dirty = true;
    return true;
}
void app_page_heading(struct app *app, unsigned index, char *heading, size_t size)
{
    heading[0] = 0;
    if (index >= PAGE_COUNT || !size)
        return;
    const char *text;
    size_t length;
    if (index == app->current) {
        text = app->editor.text;
        length = app->editor.length;
    } else {
        if (!app->root || !load(app, index))
            return;
        text = app->pages[index].text;
        length = app->pages[index].length;
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
            while (to > from &&
                   (text[to - 1] == ' ' || text[to - 1] == '\t' || text[to - 1] == '\r'))
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
unsigned app_page_modified(const struct app *app)
{
    for (unsigned i = 0; i < PAGE_COUNT; ++i)
        if (i != app->current && app->pages[i].loaded && app->pages[i].modified)
            return i + 1;
    return 0;
}
bool app_pages_save(struct app *app)
{
    struct editor *ed = &app->editor;
    unsigned saved = 0;
    for (unsigned i = 0; app->root && i < PAGE_COUNT; ++i) {
        struct page *page = &app->pages[i], *here = &app->pages[app->current];
        if (i == app->current || !page->loaded || !page->modified)
            continue;
        char *folder = page_path(app, i, NULL);
        if (!folder)
            return false;
        /* Swap the page in just to save it, with its own folder and images, as a switch
         * would, then put the shown page back. */
        editor_change_end(ed);
        bool fresh = page->fresh;
        struct media *media = page->media;
        stash(ed, here);
        here->media = app->media;
        unstash(ed, page);
        char *directory = app->directory;
        app->directory = folder;
        app->media = media;
        bool ok = editor_save(ed, ed->filename);
        stash(ed, page);
        page->fresh = fresh;
        page->media = app->media;
        app->media = here->media;
        unstash(ed, here);
        free(app->directory);
        app->directory = directory;
        if (!ok) {
            char reason[sizeof(ed->message)];
            snprintf(reason, sizeof(reason), "%s", ed->message);
            snprintf(ed->message, sizeof(ed->message), "Could not save page %u: %.200s", i + 1,
                     reason);
            return false;
        }
        ++saved;
    }
    if (saved)
        snprintf(ed->message, sizeof(ed->message), "Saved all pages");
    return true;
}
void app_pages_close(struct app *app)
{
    for (unsigned i = 0; i < PAGE_COUNT; ++i)
        if (app->pages[i].loaded)
            free_page(&app->pages[i]);
    app->current = 0;
}
