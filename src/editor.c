#include "editor.h"
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
    if (copy) memcpy(copy, value, bytes);
    return copy;
}

static bool reserve(struct editor *ed, size_t needed)
{
    if (needed <= ed->capacity) return true;
    size_t capacity = ed->capacity ? ed->capacity : 1024;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) { capacity = needed; break; }
        capacity *= 2;
    }
    char *text = realloc(ed->text, capacity);
    if (!text) { snprintf(ed->message, sizeof(ed->message), "Out of memory"); return false; }
    ed->text = text;
    ed->capacity = capacity;
    return true;
}

bool editor_open(struct editor *ed, const char *filename)
{
    if (!reserve(ed, 1)) return false;
    ed->text[0] = '\0';
    if (!filename) return true;
    ed->filename = copy_string(filename);
    if (!ed->filename) return false;
    bool missing = false;
    FILE *file = platform_file_open_read(filename, &missing, ed->message, sizeof(ed->message));
    if (!file) return missing;
    char block[4096];
    size_t count;
    while ((count = fread(block, 1, sizeof(block), file))) {
        if (memchr(block, '\0', count)) {
            snprintf(ed->message, sizeof(ed->message), "Binary file contains NUL bytes");
            fclose(file);
            return false;
        }
        if (ed->length > SIZE_MAX - count - 1 || !reserve(ed, ed->length + count + 1)) {
            fclose(file);
            return false;
        }
        memcpy(ed->text + ed->length, block, count);
        ed->length += count;
    }
    bool ok = !ferror(file);
    if (!ok) snprintf(ed->message, sizeof(ed->message), "Read: %s", strerror(errno));
    fclose(file);
    if (ok) {
        const char *first = memchr(ed->text, '\n', ed->length);
        ed->crlf = first && first > ed->text && first[-1] == '\r';
        size_t output = 0;
        for (size_t input = 0; input < ed->length; ++input) {
            if (ed->text[input] == '\r' && input + 1 < ed->length && ed->text[input + 1] == '\n') continue;
            ed->text[output++] = ed->text[input];
        }
        ed->length = output;
    }
    ed->text[ed->length] = '\0';
    return ok;
}

void editor_destroy(struct editor *ed)
{
    free(ed->text);
    free(ed->filename);
}

size_t editor_next(const struct editor *ed, size_t pos)
{
    if (pos >= ed->length) return ed->length;
    ++pos;
    while (pos < ed->length && ((unsigned char)ed->text[pos] & 0xc0) == 0x80) ++pos;
    return pos;
}

size_t editor_previous(const struct editor *ed, size_t pos)
{
    if (!pos) return 0;
    --pos;
    while (pos && ((unsigned char)ed->text[pos] & 0xc0) == 0x80) --pos;
    return pos;
}

size_t editor_line_start(const struct editor *ed, size_t pos)
{
    while (pos && ed->text[pos - 1] != '\n') --pos;
    return pos;
}

size_t editor_line_end(const struct editor *ed, size_t pos)
{
    while (pos < ed->length && ed->text[pos] != '\n') ++pos;
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
    if (c < 0x80) return c;
    unsigned count = c >= 0xf0 && c <= 0xf4 ? 3 : c >= 0xe0 && c <= 0xef ? 2 :
                     c >= 0xc2 && c <= 0xdf ? 1 : 0;
    if (!count) return 0xfffd;
    uint32_t cp = c & ((1u << (6 - count)) - 1);
    for (unsigned i = 0; i < count; ++i) {
        if (*offset >= length || ((unsigned char)text[*offset] & 0xc0) != 0x80) return 0xfffd;
        cp = (cp << 6) | ((unsigned char)text[(*offset)++] & 0x3f);
    }
    if ((count == 1 && cp < 0x80) || (count == 2 && cp < 0x800) ||
        (count == 3 && cp < 0x10000) || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
        return 0xfffd;
    return cp;
}

static void insert(struct editor *ed, const char *text)
{
    size_t count = strlen(text);
    if (ed->length > SIZE_MAX - count - 1 || !reserve(ed, ed->length + count + 1)) return;
    memmove(ed->text + ed->cursor + count, ed->text + ed->cursor, ed->length - ed->cursor + 1);
    memcpy(ed->text + ed->cursor, text, count);
    ed->cursor += count;
    ed->length += count;
    ed->modified = true;
}

static void erase(struct editor *ed, size_t from, size_t to)
{
    if (from == to) return;
    memmove(ed->text + from, ed->text + to, ed->length - to + 1);
    ed->length -= to - from;
    ed->cursor = from;
    ed->modified = true;
}

static void normal_cursor(struct editor *ed)
{
    size_t start = editor_line_start(ed, ed->cursor);
    size_t end = editor_line_end(ed, ed->cursor);
    if (ed->mode == NORMAL && ed->cursor == end && end > start)
        ed->cursor = editor_previous(ed, end);
}

static void move_vertical(struct editor *ed, bool down)
{
    size_t column = editor_column(ed, ed->cursor);
    size_t start = editor_line_start(ed, ed->cursor);
    if (down) {
        start = editor_line_end(ed, start);
        if (start == ed->length) return;
        ++start;
    } else {
        if (!start) return;
        start = editor_line_start(ed, start - 1);
    }
    size_t end = editor_line_end(ed, start), col = 0;
    ed->cursor = start;
    while (ed->cursor < end && col < column) {
        size_t advance = ed->text[ed->cursor] == '\t' ? 4 - col % 4 : 1;
        if (col + advance > column) break;
        col += advance;
        ed->cursor = editor_next(ed, ed->cursor);
    }
    normal_cursor(ed);
}

static bool save(struct editor *ed, const char *filename)
{
    if (!filename || !*filename) {
        snprintf(ed->message, sizeof(ed->message), "No filename: use :w path");
        return false;
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
        for (size_t i = 0; i < ed->length; ++i) if (ed->text[i] == '\n') ++newlines;
        if (newlines > SIZE_MAX - bytes || !(encoded = malloc(bytes + newlines + (bytes + newlines == 0)))) {
            free(new_name);
            snprintf(ed->message, sizeof(ed->message), "Out of memory encoding CRLF lines");
            return false;
        }
        size_t output = 0;
        for (size_t i = 0; i < ed->length; ++i) {
            if (ed->text[i] == '\n') encoded[output++] = '\r';
            encoded[output++] = ed->text[i];
        }
        bytes = output; data = encoded;
    }
    bool saved = platform_file_write_atomic(filename, data, bytes, ed->message, sizeof(ed->message));
    free(encoded);
    if (!saved) {
        free(new_name);
        return false;
    }
    free(ed->filename);
    ed->filename = new_name;
    ed->modified = false;
    snprintf(ed->message, sizeof(ed->message), "Written %zu bytes", bytes);
    return true;
}

static void command(struct editor *ed)
{
    char *argument = strchr(ed->command, ' ');
    if (argument) {
        *argument++ = '\0';
        while (*argument == ' ') ++argument;
    }
    if (!strcmp(ed->command, "w") || !strcmp(ed->command, "wq")) {
        bool quit = !strcmp(ed->command, "wq");
        if (save(ed, argument && *argument ? argument : ed->filename) && quit) ed->quit = true;
    } else if (!strcmp(ed->command, "q!") && !argument) ed->quit = true;
    else if (!strcmp(ed->command, "q") && !argument) {
        if (ed->modified) snprintf(ed->message, sizeof(ed->message), "Unsaved changes (:w to save, :q! to discard)");
        else ed->quit = true;
    } else snprintf(ed->message, sizeof(ed->message), "Unknown command: %.230s", ed->command);
}

void editor_input(struct editor *ed, enum editor_key key, const char *text)
{
    ed->message[0] = '\0';
    if (ed->command_active) {
        if (key == KEY_ESCAPE) ed->command_active = false;
        else if (key == KEY_ENTER) { ed->command_active = false; command(ed); }
        else if (key == KEY_BACKSPACE) {
            if (!ed->command_length) ed->command_active = false;
            else {
                do { --ed->command_length; }
                while (ed->command_length && ((unsigned char)ed->command[ed->command_length] & 0xc0) == 0x80);
                ed->command[ed->command_length] = '\0';
            }
        } else if (key == KEY_TEXT && text && *text) {
            size_t count = strlen(text);
            if (ed->command_length + count < sizeof(ed->command)) {
                memcpy(ed->command + ed->command_length, text, count + 1);
                ed->command_length += count;
            }
        }
        return;
    }
    if (key == KEY_ESCAPE) { ed->mode = NORMAL; normal_cursor(ed); return; }
    if (ed->mode == NORMAL && key == KEY_TEXT) {
        if (!strcmp(text, "i")) { ed->mode = INSERT; return; }
        else if (!strcmp(text, "h")) key = KEY_LEFT;
        else if (!strcmp(text, "j")) key = KEY_DOWN;
        else if (!strcmp(text, "k")) key = KEY_UP;
        else if (!strcmp(text, "l")) key = KEY_RIGHT;
        else if (!strcmp(text, ":")) {
            ed->command_active = true;
            ed->command_length = 0;
            ed->command[0] = '\0';
        }
    }
    if (key == KEY_LEFT) {
        if (ed->cursor > editor_line_start(ed, ed->cursor)) ed->cursor = editor_previous(ed, ed->cursor);
    } else if (key == KEY_RIGHT) {
        size_t end = editor_line_end(ed, ed->cursor);
        if (ed->cursor < end) ed->cursor = editor_next(ed, ed->cursor);
        normal_cursor(ed);
    } else if (key == KEY_UP) move_vertical(ed, false);
    else if (key == KEY_DOWN) move_vertical(ed, true);
    else if (ed->mode == INSERT) {
        if (key == KEY_ENTER) insert(ed, "\n");
        else if (key == KEY_TAB) insert(ed, "\t");
        else if (key == KEY_BACKSPACE && ed->cursor) erase(ed, editor_previous(ed, ed->cursor), ed->cursor);
        else if (key == KEY_DELETE && ed->cursor < ed->length) erase(ed, ed->cursor, editor_next(ed, ed->cursor));
        else if (key == KEY_TEXT && text && *text) insert(ed, text);
    }
}
