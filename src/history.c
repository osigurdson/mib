#include "edit-internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UNDO_BYTES (2 * 1024 * 1024)
#define UNDO_RECORDS 1024
struct delta {
    size_t at, removed, inserted, capacity, before_cursor, after_cursor;
    uint32_t group;
    char *bytes; /* removed bytes followed by inserted bytes */
};
struct edit_history {
    struct delta *deltas;
    unsigned count, position, capacity;
    uint32_t group;
    bool active, pending, overflow;
    size_t bytes;
};

uint64_t editor_hash(const struct editor *ed)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < ed->length; ++i) {
        hash ^= (unsigned char)ed->text[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

void editor_history_clear(struct editor *ed)
{
    if (!ed->history)
        return;
    for (unsigned i = 0; i < ed->history->count; ++i)
        free(ed->history->deltas[i].bytes);
    free(ed->history->deltas);
    free(ed->history);
    ed->history = NULL;
}

bool editor_change_active(const struct editor *ed) { return ed->history && ed->history->active; }
static void clear_records(struct edit_history *h)
{
    for (unsigned i = 0; i < h->count; ++i)
        free(h->deltas[i].bytes);
    h->count = h->position = 0;
    h->bytes = 0;
    h->pending = true;
}
void editor_change_begin(struct editor *ed)
{
    if (!ed->history)
        ed->history = calloc(1, sizeof(*ed->history));
    if (ed->history && !ed->history->active) {
        ++ed->history->group;
        ed->history->active = true;
        ed->history->pending = true;
        ed->history->overflow = false;
    }
}

void editor_change_end(struct editor *ed)
{
    struct edit_history *h = ed->history;
    if (!h)
        return;
    if (h->count && !h->pending)
        h->deltas[h->count - 1].after_cursor = ed->cursor;
    h->active = false;
}

bool editor_raw_replace(struct editor *ed, size_t from, size_t to, const char *text, size_t length)
{
    if (from > to || to > ed->length || length > SIZE_MAX - (ed->length - (to - from)) - 1)
        return false;
    size_t total = ed->length - (to - from) + length;
    if (!editor_reserve(ed, total + 1))
        return false;
    memmove(ed->text + from + length, ed->text + to, ed->length - to);
    if (length)
        memcpy(ed->text + from, text, length);
    ed->text[total] = 0;
    ed->length = total;
    ed->cursor = from + length;
    ed->modified = true;
    ++ed->revision;
    return true;
}

static void discard_tail(struct edit_history *h)
{
    while (h->count > h->position) {
        struct delta *d = &h->deltas[--h->count];
        h->bytes -= d->capacity;
        free(d->bytes);
    }
}

static void evict(struct edit_history *h)
{
    if (!h->count)
        return;
    uint32_t group = h->deltas[0].group;
    unsigned n = 0;
    while (n < h->count && h->deltas[n].group == group) {
        h->bytes -= h->deltas[n].capacity;
        free(h->deltas[n++].bytes);
    }
    memmove(h->deltas, h->deltas + n, (h->count - n) * sizeof(*h->deltas));
    h->count -= n;
    h->position = h->position > n ? h->position - n : 0;
}

bool editor_replace(struct editor *ed, size_t from, size_t to, const char *text, size_t length)
{
    if (from > to || to > ed->length || length > SIZE_MAX - (ed->length - (to - from)) - 1)
        return false;
    if (to - from == length && (!length || !memcmp(ed->text + from, text, length)))
        return true;
    if (!editor_reserve(ed, ed->length - (to - from) + length + 1))
        return false;
    bool own = !ed->history || !ed->history->active;
    if (own)
        editor_change_begin(ed);
    struct edit_history *h = ed->history;
    if (!h) {
        snprintf(ed->message, sizeof(ed->message), "Out of memory for undo");
        return false;
    }
    discard_tail(h);
    size_t bytes = to - from + length;
    if (h->overflow || bytes > UNDO_BYTES) {
        if (!h->overflow) {
            clear_records(h);
            h->overflow = true;
            snprintf(ed->message, sizeof(ed->message),
                     "Large change: undo history cleared (2 MiB budget)");
        }
        bool ok = editor_raw_replace(ed, from, to, text, length);
        if (own)
            editor_change_end(ed);
        return ok;
    }
    struct delta *last = h->count ? &h->deltas[h->count - 1] : NULL;
    if (last && last->group == h->group && !last->removed && to == from &&
        from == last->at + last->inserted && last->inserted + length <= UNDO_BYTES) {
        size_t need = last->inserted + length;
        if (need > last->capacity) {
            size_t capacity = last->capacity ? last->capacity : 32;
            while (capacity < need)
                capacity *= 2;
            if (h->bytes - last->capacity + capacity > UNDO_BYTES)
                goto new_record;
            char *data = realloc(last->bytes, capacity);
            if (!data)
                return false;
            h->bytes += capacity - last->capacity;
            last->capacity = capacity;
            last->bytes = data;
        }
        memcpy(last->bytes + last->inserted, text, length);
        last->inserted += length;
        bool ok = editor_raw_replace(ed, from, to, text, length);
        last->after_cursor = ed->cursor;
        if (own)
            editor_change_end(ed);
        return ok;
    }
new_record:
    while (h->count && (h->bytes + bytes > UNDO_BYTES || h->count == UNDO_RECORDS)) {
        if (h->deltas[0].group == h->group) {
            clear_records(h);
            h->overflow = true;
            bool ok = editor_raw_replace(ed, from, to, text, length);
            snprintf(ed->message, sizeof(ed->message),
                     "Insert session exceeds undo budget; history cleared");
            if (own)
                editor_change_end(ed);
            return ok;
        }
        evict(h);
    }
    if (h->count == h->capacity) {
        unsigned n = h->capacity ? h->capacity * 2 : 16;
        struct delta *p = realloc(h->deltas, n * sizeof(*p));
        if (!p)
            return false;
        h->deltas = p;
        h->capacity = n;
    }
    char *copy = malloc(bytes ? bytes : 1);
    if (!copy)
        return false;
    memcpy(copy, ed->text + from, to - from);
    if (length)
        memcpy(copy + to - from, text, length);
    struct delta d = {from, to - from, length, bytes, ed->cursor, from + length, h->group, copy};
    h->deltas[h->count++] = d;
    h->position = h->count;
    h->bytes += bytes;
    h->pending = false;
    bool ok = editor_raw_replace(ed, from, to, text, length);
    if (own)
        editor_change_end(ed);
    return ok;
}

void editor_undo(struct editor *ed, bool redo)
{
    editor_change_end(ed);
    struct edit_history *h = ed->history;
    if (!h || (redo ? h->position == h->count : !h->position)) {
        snprintf(ed->message, sizeof(ed->message), "Already at %s change",
                 redo ? "newest" : "oldest");
        return;
    }
    uint32_t group = h->deltas[redo ? h->position : h->position - 1].group;
    do {
        struct delta *d = &h->deltas[redo ? h->position : h->position - 1];
        bool ok = redo ? editor_raw_replace(ed, d->at, d->at + d->removed, d->bytes + d->removed,
                                            d->inserted)
                       : editor_raw_replace(ed, d->at, d->at + d->inserted, d->bytes, d->removed);
        if (!ok)
            break;
        ed->cursor = redo ? d->after_cursor : d->before_cursor;
        if (redo)
            ++h->position;
        else
            --h->position;
    } while (redo ? (h->position < h->count && h->deltas[h->position].group == group)
                  : (h->position && h->deltas[h->position - 1].group == group));
    ed->modified = editor_hash(ed) != ed->saved_hash;
    editor_clamp(ed);
}
