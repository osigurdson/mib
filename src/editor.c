#include "edit-internal.h"
#include "platform.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *copy_string(const char *value)
{
    size_t bytes = strlen(value) + 1;
    char *copy = malloc(bytes);
    if (copy)
        memcpy(copy, value, bytes);
    return copy;
}

bool editor_can_edit(struct editor *ed)
{
    if (ed->view != VIEW_READING)
        return true;
    snprintf(ed->message, sizeof(ed->message),
             "Reading view is read-only (:set view=normal to edit)");
    return false;
}

bool editor_reserve(struct editor *ed, size_t needed)
{
    if (needed <= ed->capacity)
        return true;
    size_t capacity = ed->capacity ? ed->capacity : 1024;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) {
            capacity = needed;
            break;
        }
        capacity *= 2;
    }
    char *text = realloc(ed->text, capacity);
    if (!text) {
        snprintf(ed->message, sizeof(ed->message), "Out of memory");
        return false;
    }
    ed->text = text;
    ed->capacity = capacity;
    return true;
}

bool editor_open(struct editor *ed, const char *filename)
{
    ed->view = VIEW_NORMAL;
    ed->number = true;
    ed->viewport_rows = 24;
    ed->viewport_columns = 80;
    if (!editor_reserve(ed, 1))
        return false;
    ed->saved_hash = UINT64_C(14695981039346656037);
    ed->text[0] = '\0';
    if (!filename)
        return true;
    ed->filename = copy_string(filename);
    if (!ed->filename)
        return false;
    bool missing = false;
    FILE *file = platform_file_open_read(filename, &missing, ed->message, sizeof(ed->message));
    if (!file)
        return missing;
    char block[4096];
    size_t count;
    while ((count = fread(block, 1, sizeof(block), file))) {
        if (memchr(block, '\0', count)) {
            snprintf(ed->message, sizeof(ed->message), "Binary file contains NUL bytes");
            fclose(file);
            return false;
        }
        if (ed->length > SIZE_MAX - count - 1 || !editor_reserve(ed, ed->length + count + 1)) {
            fclose(file);
            return false;
        }
        memcpy(ed->text + ed->length, block, count);
        ed->length += count;
    }
    bool ok = !ferror(file);
    if (!ok)
        snprintf(ed->message, sizeof(ed->message), "Read: %s", strerror(errno));
    fclose(file);
    if (ok) {
        const char *first = memchr(ed->text, '\n', ed->length);
        ed->crlf = first && first > ed->text && first[-1] == '\r';
        size_t output = 0;
        for (size_t input = 0; input < ed->length; ++input) {
            if (ed->text[input] == '\r' && input + 1 < ed->length && ed->text[input + 1] == '\n')
                continue;
            ed->text[output++] = ed->text[input];
        }
        ed->length = output;
    }
    ed->text[ed->length] = '\0';
    ed->saved_hash = editor_hash(ed);
    return ok;
}

void editor_destroy(struct editor *ed)
{
    editor_history_clear(ed);
    free(ed->register_text);
    free(ed->dot);
    free(ed->replace_original);
    free(ed->dot_record);
    for (unsigned i = 0; i < sizeof(ed->registers) / sizeof(ed->registers[0]); ++i)
        free(ed->registers[i].text);
    for (unsigned i = 0; i < ed->history_count; ++i)
        free(ed->command_history[i]);
    free(ed->text);
    free(ed->filename);
}

size_t editor_next(const struct editor *ed, size_t pos)
{
    if (pos >= ed->length)
        return ed->length;
    ++pos;
    while (pos < ed->length && ((unsigned char)ed->text[pos] & 0xc0) == 0x80)
        ++pos;
    return pos;
}

size_t editor_previous(const struct editor *ed, size_t pos)
{
    if (!pos)
        return 0;
    --pos;
    while (pos && ((unsigned char)ed->text[pos] & 0xc0) == 0x80)
        --pos;
    return pos;
}

size_t editor_line_start(const struct editor *ed, size_t pos)
{
    while (pos && ed->text[pos - 1] != '\n')
        --pos;
    return pos;
}

size_t editor_line_end(const struct editor *ed, size_t pos)
{
    while (pos < ed->length && ed->text[pos] != '\n')
        ++pos;
    return pos;
}

size_t editor_column(const struct editor *ed, size_t pos)
{
    size_t column = 0;
    for (size_t i = editor_line_start(ed, pos); i < pos; i = editor_next(ed, i))
        column += ed->text[i] == '\t' ? 4 - column % 4 : 1;
    return column;
}

uint32_t editor_codepoint(const char *text, size_t length, size_t *offset)
{
    unsigned char c = (unsigned char)text[(*offset)++];
    if (c < 0x80)
        return c;
    unsigned count = c >= 0xf0 && c <= 0xf4   ? 3
                     : c >= 0xe0 && c <= 0xef ? 2
                     : c >= 0xc2 && c <= 0xdf ? 1
                                              : 0;
    if (!count)
        return 0xfffd;
    uint32_t cp = c & ((1u << (6 - count)) - 1);
    for (unsigned i = 0; i < count; ++i) {
        if (*offset >= length || ((unsigned char)text[*offset] & 0xc0) != 0x80)
            return 0xfffd;
        cp = (cp << 6) | ((unsigned char)text[(*offset)++] & 0x3f);
    }
    if ((count == 1 && cp < 0x80) || (count == 2 && cp < 0x800) || (count == 3 && cp < 0x10000) ||
        cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
        return 0xfffd;
    return cp;
}

bool editor_save(struct editor *ed, const char *filename)
{
    return editor_write(ed, filename, false);
}
bool editor_write(struct editor *ed, const char *filename, bool force)
{
    if (!filename || !*filename) {
        snprintf(ed->message, sizeof(ed->message), "No filename: open a page or file to save");
        return false;
    }
    if (!force && ed->filename && !strcmp(filename, ed->filename)) {
        /* The file should still hold what was last read or saved (saved_hash). It may also
         * already match this text, in which case nothing would be lost. */
        struct editor disk = {0};
        bool changed = editor_open(&disk, filename) && disk.saved_hash != ed->saved_hash &&
                       disk.saved_hash != editor_hash(ed);
        editor_destroy(&disk);
        if (changed) {
            snprintf(ed->message, sizeof(ed->message),
                     "Changed on disk since it was read (:e! to reload, :w! to overwrite)");
            return false;
        }
    }
    char *new_name = copy_string(filename);
    if (!new_name) {
        snprintf(ed->message, sizeof(ed->message), "Out of memory");
        return false;
    }
    const char *data = ed->text;
    char *encoded = NULL;
    size_t bytes = ed->length;
    if (ed->crlf) {
        size_t newlines = 0;
        for (size_t i = 0; i < ed->length; ++i)
            if (ed->text[i] == '\n')
                ++newlines;
        if (newlines > SIZE_MAX - bytes ||
            !(encoded = malloc(bytes + newlines + (bytes + newlines == 0)))) {
            free(new_name);
            snprintf(ed->message, sizeof(ed->message), "Out of memory encoding CRLF lines");
            return false;
        }
        size_t output = 0;
        for (size_t i = 0; i < ed->length; ++i) {
            if (ed->text[i] == '\n')
                encoded[output++] = '\r';
            encoded[output++] = ed->text[i];
        }
        bytes = output;
        data = encoded;
    }
    bool saved =
        platform_file_write_atomic(filename, data, bytes, ed->message, sizeof(ed->message));
    free(encoded);
    if (!saved) {
        free(new_name);
        return false;
    }
    free(ed->filename);
    ed->filename = new_name;
    ed->modified = false;
    ed->saved_hash = editor_hash(ed);
    snprintf(ed->message, sizeof(ed->message), "Written %zu bytes", bytes);
    if (ed->on_save)
        ed->on_save(ed->on_save_data);
    return true;
}

size_t editor_line_at(const struct editor *ed, size_t line)
{
    size_t p = 0;
    while (line-- && p < ed->length) {
        p = editor_line_end(ed, p);
        if (p < ed->length)
            ++p;
    }
    return p;
}
size_t editor_line_number(const struct editor *ed, size_t pos)
{
    size_t n = 0;
    for (size_t i = 0; i < pos && i < ed->length; ++i)
        n += ed->text[i] == '\n';
    return n;
}
size_t editor_line_count(const struct editor *ed) { return editor_line_number(ed, ed->length) + 1; }
void editor_clamp(struct editor *ed)
{
    if (ed->cursor > ed->length)
        ed->cursor = ed->length;
    size_t start = editor_line_start(ed, ed->cursor), end = editor_line_end(ed, ed->cursor);
    if (ed->mode != INSERT && ed->cursor == end && end > start)
        ed->cursor = editor_previous(ed, end);
}
bool editor_reload(struct editor *ed)
{
    if (!ed->filename)
        return false;
    struct editor other = {0};
    if (!editor_open(&other, ed->filename)) {
        snprintf(ed->message, sizeof(ed->message), "%s", other.message);
        editor_destroy(&other);
        return false;
    }
    free(ed->text);
    ed->text = other.text;
    other.text = NULL;
    ed->length = other.length;
    ed->capacity = other.capacity;
    ed->crlf = other.crlf;
    ed->saved_hash = other.saved_hash;
    ed->modified = false;
    ++ed->revision;
    memset(ed->marks, 0, sizeof(ed->marks));
    ed->context = 0;
    ed->jump_count = ed->jump_index = 0;
    editor_history_clear(ed);
    editor_destroy(&other);
    editor_clamp(ed);
    return true;
}

char editor_fence(const struct editor *ed, size_t p)
{
    size_t start = p, end = editor_line_end(ed, p);
    while (p < end && p - start < 3 && ed->text[p] == ' ')
        ++p;
    if (p + 3 > end || (ed->text[p] != '`' && ed->text[p] != '~'))
        return 0;
    return ed->text[p] == ed->text[p + 1] && ed->text[p] == ed->text[p + 2] ? ed->text[p] : 0;
}
bool editor_heading(const struct editor *ed, size_t p)
{
    size_t start = editor_line_start(ed, p), end = editor_line_end(ed, p);
    char fenced = 0;
    if (start == end || ed->text[start] != '#')
        return false;
    size_t at = start;
    while (at < end && ed->text[at] == '#')
        ++at;
    if (at - start > 6 || at == end || ed->text[at] != ' ')
        return false;
    for (size_t line = 0; line < start;) {
        char marker = editor_fence(ed, line);
        if (marker)
            fenced = fenced ? (fenced == marker ? 0 : fenced) : marker;
        line = editor_line_end(ed, line) + 1;
    }
    return !fenced;
}
