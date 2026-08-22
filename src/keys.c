/* Small modal editor. No per-keystroke heap work except buffer/undo growth. */
#include "edit-internal.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t min(size_t a, size_t b) { return a < b ? a : b; }
static size_t max(size_t a, size_t b) { return a > b ? a : b; }
static bool space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
static int word(unsigned char c, bool big)
{
    return space((char)c) ? 0 : big || isalnum(c) || c == '_' || c >= 128 ? 1 : 2;
}
static size_t first_nonblank(const struct editor *ed, size_t p)
{
    p = editor_line_start(ed, p);
    while (p < ed->length && (ed->text[p] == ' ' || ed->text[p] == '\t'))
        ++p;
    return p;
}
static size_t vertical(struct editor *ed, size_t p, long lines)
{
    size_t col = editor_column(ed, p), line = editor_line_number(ed, p),
           total = editor_line_count(ed);
    if (lines < 0)
        line -= min(line, (size_t)-lines);
    else
        line = min(total - 1, line + (size_t)lines);
    p = editor_line_at(ed, line);
    size_t c = 0, end = editor_line_end(ed, p);
    while (p < end && c < col) {
        size_t step = ed->text[p] == '\t' ? 4 - c % 4 : 1;
        if (c + step > col)
            break;
        c += step;
        p = editor_next(ed, p);
    }
    return p;
}
bool editor_selection(const struct editor *ed, size_t *a, size_t *b, bool *linewise)
{
    if (ed->mode != VISUAL && ed->mode != VISUAL_LINE)
        return false;
    *a = min(ed->anchor, ed->cursor);
    *b = max(ed->anchor, ed->cursor);
    *linewise = ed->mode == VISUAL_LINE;
    if (*linewise) {
        *a = editor_line_start(ed, *a);
        *b = editor_line_end(ed, *b);
        if (*b < ed->length)
            ++*b;
    } else
        *b = editor_next(ed, *b);
    return true;
}
/* "0 is slot 0 and "a to "z (or "A to "Z, which append) are slots 1 to 26. */
static int register_slot(char name)
{
    if (name == '0')
        return 0;
    if (name >= 'a' && name <= 'z')
        return 1 + name - 'a';
    if (name >= 'A' && name <= 'Z')
        return 1 + name - 'A';
    return -1;
}
static struct editor_register *named_register(struct editor *ed, char name)
{
    int slot = register_slot(name);
    return slot < 0 ? NULL : &ed->registers[slot];
}
static bool register_supported(char name)
{
    return register_slot(name) >= 0 || (name && strchr("\"_+*", name));
}
/* Store or append text; appending mixes characterwise and linewise text the way Vim does. */
static bool store(struct editor_register *r, const char *text, size_t n, bool linewise,
                  bool append)
{
    size_t old = append && r->text ? r->length : 0;
    bool before = old && !r->linewise && linewise, after = old && r->linewise && !linewise;
    char *data = malloc(old + before + n + after + 1);
    if (!data)
        return false;
    if (old)
        memcpy(data, r->text, old);
    if (before)
        data[old] = '\n';
    memcpy(data + old + before, text, n);
    if (after)
        data[old + before + n] = '\n';
    free(r->text);
    r->text = data;
    r->length = old + before + n + after;
    r->text[r->length] = 0;
    r->linewise = old ? r->linewise || linewise : linewise;
    return true;
}
void editor_yank(struct editor *ed, size_t a, size_t b, bool linewise, bool yank)
{
    if (b <= a || ed->register_name == '_')
        return;
    char *copy = malloc(b - a + 2);
    if (!copy)
        return;
    memcpy(copy, ed->text + a, b - a);
    size_t n = b - a;
    if (linewise && copy[n - 1] != '\n')
        copy[n++] = '\n';
    copy[n] = 0;
    struct editor_register *named = named_register(ed, ed->register_name);
    if (named) {
        bool ok = store(named, copy, n, linewise, isupper((unsigned char)ed->register_name));
        free(copy);
        /* Like Vim, the unnamed register also gets what was stored. */
        if (!ok || !(copy = malloc(named->length + 1)))
            return;
        memcpy(copy, named->text, named->length + 1);
        n = named->length;
        linewise = named->linewise;
    } else if (yank)
        store(&ed->registers[0], copy, n, linewise, false);
    free(ed->register_text);
    ed->register_text = copy;
    ed->register_length = n;
    ed->register_linewise = linewise;
    ed->requests |= ED_COPY;
}
void editor_forget_registers(struct editor *ed, const char *text)
{
    if (ed->register_text && strstr(ed->register_text, text)) {
        free(ed->register_text);
        ed->register_text = NULL;
        ed->register_length = 0;
    }
    for (unsigned i = 0; i < sizeof(ed->registers) / sizeof(ed->registers[0]); ++i)
        if (ed->registers[i].text && strstr(ed->registers[i].text, text)) {
            free(ed->registers[i].text);
            ed->registers[i] = (struct editor_register){0};
        }
}
static size_t shifted(size_t mark, size_t from, size_t to, size_t length)
{
    if (!mark || mark - 1 < from)
        return mark;
    if (mark - 1 >= to)
        return mark - (to - from) + length;
    return from + 1; /* Inside the replaced text: keep it at the start. */
}
void editor_shift_marks(struct editor *ed, size_t from, size_t to, size_t length)
{
    for (unsigned i = 0; i < 26; ++i)
        ed->marks[i] = shifted(ed->marks[i], from, to, length);
    ed->context = shifted(ed->context, from, to, length);
    for (unsigned i = 0; i < ed->jump_count; ++i)
        ed->jumps[i] = shifted(ed->jumps[i], from, to, length);
}
/* Append a jump-list entry. A new jump drops older entries on its line, as Vim does; the spot
 * Ctrl-O saves before stepping back keeps them. */
static void push_jump(struct editor *ed, size_t position, bool dedupe)
{
    size_t line = editor_line_number(ed, position);
    unsigned kept = 0;
    for (unsigned i = 0; i < ed->jump_count; ++i)
        if (!dedupe || editor_line_number(ed, min(ed->jumps[i] - 1, ed->length)) != line)
            ed->jumps[kept++] = ed->jumps[i];
    if (kept == sizeof(ed->jumps) / sizeof(ed->jumps[0])) {
        memmove(ed->jumps, ed->jumps + 1, (kept - 1) * sizeof(ed->jumps[0]));
        --kept;
    }
    ed->jumps[kept++] = position + 1;
    ed->jump_count = ed->jump_index = kept;
}
void editor_jump_from(struct editor *ed, size_t origin)
{
    ed->context = origin + 1;
    push_jump(ed, origin, true);
}
/* Ctrl-O and Ctrl-I: walk the jump list, saving the current spot before the first step back. */
static void jump_list(struct editor *ed, bool older)
{
    if (older) {
        if (ed->jump_index == ed->jump_count) {
            push_jump(ed, ed->cursor, false);
            ed->jump_index = ed->jump_count - 1;
        }
        if (!ed->jump_index)
            return;
        --ed->jump_index;
    } else {
        if (ed->jump_index + 1 >= ed->jump_count)
            return;
        ++ed->jump_index;
    }
    ed->cursor = min(ed->jumps[ed->jump_index] - 1, ed->length);
    editor_clamp(ed);
}
static bool jump_motion(const char *m)
{
    return strchr("GHLM%{}nN'`", m[0]) || !strcmp(m, "gg") || !strcmp(m, "[[") ||
           !strcmp(m, "]]");
}

/* p and P: named registers are put directly; the unnamed one goes through the clipboard. */
static void put(struct editor *ed, bool after, unsigned count)
{
    struct editor_register *named = named_register(ed, ed->register_name);
    if (ed->register_name == '_')
        return;
    if (named) {
        if (!named->text)
            snprintf(ed->message, sizeof(ed->message), "Nothing in register %c",
                     ed->register_name);
        else
            editor_put(ed, named->text, named->length, named->linewise, after, count);
        return;
    }
    ed->put_after = after;
    ed->put_count = count;
    ed->requests |= ED_PASTE;
}
void editor_put(struct editor *ed, const char *text, size_t length, bool linewise, bool after,
                unsigned count)
{
    if (!editor_can_edit(ed))
        return;
    if (!length || !count)
        return;
    if (length > 16 * 1024 * 1024 || count > 16 * 1024 * 1024 / length) {
        snprintf(ed->message, sizeof(ed->message), "Paste exceeds 16 MiB limit");
        return;
    }
    size_t p = ed->cursor;
    size_t selection_a, selection_b;
    bool selection_lines;
    bool visual = editor_selection(ed, &selection_a, &selection_b, &selection_lines);
    bool insert_mode = ed->mode == INSERT;
    bool own = !editor_change_active(ed);
    editor_change_begin(ed);
    if (visual) {
        p = selection_a;
        editor_replace(ed, selection_a, selection_b, "", 0);
        ed->mode = NORMAL;
    }
    if (!insert_mode && !visual) {
        if (linewise) {
            p = after ? editor_line_end(ed, p) : editor_line_start(ed, p);
            if (after && p < ed->length)
                ++p;
            else if (after && p && ed->text[p - 1] != '\n') {
                editor_replace(ed, p, p, "\n", 1);
                ++p;
            }
        } else if (after && p < editor_line_end(ed, p))
            p = editor_next(ed, p);
    }
    size_t start = p;
    while (count--) {
        if (!editor_replace(ed, p, p, text, length))
            break;
        p += length;
    }
    ed->cursor = insert_mode ? p : linewise ? first_nonblank(ed, start) : editor_previous(ed, p);
    if (own)
        editor_change_end(ed);
    editor_clamp(ed);
}
void editor_indent(struct editor *ed, size_t a, size_t b, bool right)
{
    a = editor_line_start(ed, a);
    b = editor_line_end(ed, b);
    bool own = !editor_change_active(ed);
    editor_change_begin(ed);
    size_t p = a;
    while (p <= b && p <= ed->length) {
        size_t end = editor_line_end(ed, p);
        if (right) {
            if (!editor_replace(ed, p, p, "    ", 4))
                break;
            end += 4;
            b += 4;
        } else {
            size_t n = 0;
            if (p < ed->length && ed->text[p] == '\t')
                n = 1;
            else
                while (n < 4 && p + n < end && ed->text[p + n] == ' ')
                    ++n;
            if (n) {
                editor_replace(ed, p, p + n, "", 0);
                end -= n;
                b -= n;
            }
        }
        if (end == ed->length)
            break;
        p = end + 1;
    }
    ed->cursor = first_nonblank(ed, a);
    if (own)
        editor_change_end(ed);
}
void editor_join(struct editor *ed, size_t a, size_t b)
{
    a = editor_line_start(ed, a);
    b = editor_line_end(ed, b);
    bool own = !editor_change_active(ed);
    editor_change_begin(ed);
    size_t p = editor_line_end(ed, a), cursor = p;
    while (p < b && p < ed->length) {
        size_t end = p + 1;
        while (end < ed->length && (ed->text[end] == ' ' || ed->text[end] == '\t'))
            ++end;
        const char *join =
            p && ed->text[p - 1] != ' ' && end < ed->length && ed->text[end] != '\n' ? " " : "";
        size_t n = strlen(join), removed = end - p;
        editor_replace(ed, p, end, join, n);
        b -= removed - n;
        p = editor_line_end(ed, p);
    }
    ed->cursor = cursor;
    editor_clamp(ed);
    if (own)
        editor_change_end(ed);
}
static void case_range(struct editor *ed, size_t a, size_t b, char how)
{
    editor_change_begin(ed);
    for (size_t p = a; p < b; ++p) {
        unsigned char c = (unsigned char)ed->text[p];
        if (c >= 128)
            continue;
        char out = how == 'u'   ? (char)tolower(c)
                   : how == 'U' ? (char)toupper(c)
                   : isupper(c) ? (char)tolower(c)
                                : (char)toupper(c);
        if (out != (char)c)
            editor_replace(ed, p, p + 1, &out, 1);
    }
    ed->cursor = a;
    editor_change_end(ed);
}
bool editor_match_at(const struct editor *ed, size_t p)
{
    size_t n = strlen(ed->search);
    if (!n || n > ed->length - p)
        return false;
    bool sensitive = false;
    for (size_t i = 0; i < n; ++i)
        if (isupper((unsigned char)ed->search[i]))
            sensitive = true;
    for (size_t i = 0; i < n; ++i) {
        unsigned char a = (unsigned char)ed->text[p + i], b = (unsigned char)ed->search[i];
        if (sensitive ? a != b : tolower(a) != tolower(b))
            return false;
    }
    if (ed->search_word) {
        if (p && word((unsigned char)ed->text[p - 1], false) == 1)
            return false;
        if (p + n < ed->length && word((unsigned char)ed->text[p + n], false) == 1)
            return false;
    }
    return true;
}
void editor_search(struct editor *ed, bool forward, unsigned count)
{
    ed->search_hidden = false;
    if (!ed->search[0] || !ed->length)
        return;
    ++ed->search_serial;
    ed->search_failed = false;
    size_t p = ed->cursor;
    while (count--) {
        size_t origin = p;
        size_t extra = ed->extra_search
                           ? ed->extra_search(ed, origin, forward, ed->extra_search_data)
                           : SIZE_MAX;
        bool found = false;
        for (size_t steps = 0; steps <= ed->length; ++steps) {
            p = forward ? (p == ed->length ? 0 : editor_next(ed, p))
                        : (p ? editor_previous(ed, p) : ed->length);
            if (editor_match_at(ed, p)) {
                found = true;
                break;
            }
        }
        if (extra != SIZE_MAX) {
            size_t distance =
                forward ? (extra > origin ? extra - origin : ed->length - origin + extra + 1)
                        : (extra < origin ? origin - extra : origin + ed->length - extra + 1);
            size_t text_distance = forward
                                       ? (p > origin ? p - origin : ed->length - origin + p + 1)
                                       : (p < origin ? origin - p : origin + ed->length - p + 1);
            if (!found || distance < text_distance) {
                p = extra;
                found = true;
            }
        }
        if (!found) {
            ed->search_failed = true;
            snprintf(ed->message, sizeof(ed->message), "Pattern not found: %.180s%s", ed->search,
                     ed->search_pending ? " (still indexing image)" : "");
            return;
        }
    }
    ed->cursor = ed->match_start = p;
    ed->match_end = p + strlen(ed->search);
}
static size_t motion(struct editor *ed, const char *m, unsigned count, bool explicit_count,
                     bool *linewise, bool *inclusive, bool *valid)
{
    size_t p = ed->cursor, end;
    char c = *m;
    *linewise = *inclusive = false;
    *valid = true;
    if ((c == '\'' || c == '`') && m[1]) {
        size_t mark = m[1] == '\'' || m[1] == '`'   ? ed->context
                      : m[1] >= 'a' && m[1] <= 'z' ? ed->marks[m[1] - 'a']
                                                   : 0;
        *valid = mark != 0;
        if (!mark) {
            snprintf(ed->message, sizeof(ed->message), "Mark not set: %.4s", m + 1);
            return p;
        }
        p = min(mark - 1, ed->length);
        if (c == '\'') {
            *linewise = true;
            return first_nonblank(ed, p);
        }
        return p;
    }
    if (c == 'G' || !strcmp(m, "gg")) {
        *linewise = true;
        return editor_line_at(ed,
                              c == 'G' && !explicit_count ? editor_line_count(ed) - 1 : count - 1);
    }
    if (c == 'H' || c == 'M' || c == 'L') {
        size_t line = editor_line_number(ed, ed->top),
               rows = ed->viewport_rows ? ed->viewport_rows : 24;
        line += c == 'H' ? count - 1 : c == 'M' ? rows / 2 : rows > count ? rows - count : 0;
        *linewise = true;
        return first_nonblank(ed, editor_line_at(ed, line));
    }
    if (c == 'j' || c == 'k') {
        *linewise = true;
        return vertical(ed, p, c == 'j' ? (long)count : -(long)count);
    }
    if (c == '0')
        return editor_line_start(ed, p);
    if (c == '^')
        return first_nonblank(ed, p);
    if (c == '$' || !strcmp(m, "g_")) {
        p = vertical(ed, p, count - 1);
        end = editor_line_end(ed, p);
        *inclusive = true;
        if (m[0] == 'g')
            while (end > editor_line_start(ed, p) && space(ed->text[end - 1]))
                --end;
        return end > editor_line_start(ed, p) ? editor_previous(ed, end) : end;
    }
    if (c == '%') {
        const char *opens = "([{", *closes = ")]}", *which = NULL;
        end = editor_line_end(ed, p);
        while (p < end && !(which = strchr("()[]{}", ed->text[p])))
            ++p;
        if (!which) {
            *valid = false;
            return ed->cursor;
        }
        char ch = ed->text[p], other;
        int dir, depth = 1;
        const char *q = strchr(opens, ch);
        if (q) {
            dir = 1;
            other = closes[q - opens];
        } else {
            q = strchr(closes, ch);
            dir = -1;
            other = opens[q - closes];
        }
        while ((dir > 0 && p + 1 < ed->length) || (dir < 0 && p)) {
            p = dir > 0 ? editor_next(ed, p) : editor_previous(ed, p);
            if (ed->text[p] == ch)
                ++depth;
            if (ed->text[p] == other && !--depth) {
                *inclusive = true;
                return p;
            }
        }
        *valid = false;
        return ed->cursor;
    }
    if (c == 'n' || c == 'N') {
        size_t old = ed->cursor;
        editor_search(ed, c == 'n' ? ed->search_forward : !ed->search_forward, count);
        p = ed->cursor;
        ed->cursor = old;
        return p;
    }
    if (c == ';' || c == ',') {
        char seq[8] = {ed->find_kind};
        memcpy(seq + 1, ed->find_text, sizeof(ed->find_text));
        if (c == ',')
            seq[0] = seq[0] == 'f' ? 'F' : seq[0] == 'F' ? 'f' : seq[0] == 't' ? 'T' : 't';
        uint32_t cp = ed->find_char;
        if (!cp) {
            *valid = false;
            return p;
        }
        char kind = ed->find_kind;
        uint32_t original = ed->find_char;
        size_t result = motion(ed, seq, count, false, linewise, inclusive, valid);
        ed->find_kind = kind;
        ed->find_char = original;
        return result;
    }
    if (strchr("fFtT", c) && m[1]) {
        size_t offset = 1;
        uint32_t cp = editor_codepoint(m, strlen(m), &offset);
        ed->find_char = cp;
        size_t bytes = offset - 1;
        if (bytes > 4)
            bytes = 4;
        memcpy(ed->find_text, m + 1, bytes);
        ed->find_text[bytes] = 0;
        ed->find_kind = c;
        bool forward = c == 'f' || c == 't';
        size_t start = editor_line_start(ed, p);
        end = editor_line_end(ed, p);
        while (count--) {
            bool found = false;
            while (forward ? p < end : p > start) {
                p = forward ? editor_next(ed, p) : editor_previous(ed, p);
                if (p >= end)
                    break;
                offset = p;
                if (editor_codepoint(ed->text, ed->length, &offset) == cp) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                *valid = false;
                return ed->cursor;
            }
        }
        if (c == 't')
            p = editor_previous(ed, p);
        if (c == 'T')
            p = editor_next(ed, p);
        *inclusive = forward;
        return p;
    }
    while (count--) {
        if (c == 'h') {
            if (p > editor_line_start(ed, p))
                p = editor_previous(ed, p);
        } else if (c == 'l') {
            if (p < editor_line_end(ed, p))
                p = editor_next(ed, p);
        } else if (strchr("wWbBeE", c)) {
            bool big = isupper((unsigned char)c);
            char lower = (char)tolower((unsigned char)c);
            if (lower == 'b') {
                if (p)
                    p = editor_previous(ed, p);
                while (p && !word((unsigned char)ed->text[p], big))
                    p = editor_previous(ed, p);
                int kind = p < ed->length ? word((unsigned char)ed->text[p], big) : 0;
                while (p && word((unsigned char)ed->text[editor_previous(ed, p)], big) == kind)
                    p = editor_previous(ed, p);
            } else if (lower == 'w') {
                int kind = p < ed->length ? word((unsigned char)ed->text[p], big) : 0;
                while (p < ed->length && word((unsigned char)ed->text[p], big) == kind)
                    p = editor_next(ed, p);
                while (p < ed->length && !word((unsigned char)ed->text[p], big))
                    p = editor_next(ed, p);
            } else {
                if (p < ed->length)
                    p = editor_next(ed, p);
                while (p < ed->length && !word((unsigned char)ed->text[p], big))
                    p = editor_next(ed, p);
                int kind = p < ed->length ? word((unsigned char)ed->text[p], big) : 0;
                while (p < ed->length && editor_next(ed, p) < ed->length &&
                       word((unsigned char)ed->text[editor_next(ed, p)], big) == kind)
                    p = editor_next(ed, p);
                *inclusive = true;
            }
        } else if (c == '{' || c == '}') {
            bool forward = c == '}';
            do {
                size_t s = editor_line_start(ed, p);
                if (forward) {
                    p = editor_line_end(ed, p);
                    if (p < ed->length)
                        ++p;
                    else
                        break;
                } else {
                    if (!s) {
                        p = 0;
                        break;
                    }
                    p = editor_line_start(ed, s - 1);
                }
            } while (editor_line_end(ed, p) > p);
        } else if (!strcmp(m, "]]") || !strcmp(m, "[[")) {
            bool forward = c == ']';
            do {
                size_t s = editor_line_start(ed, p);
                if (forward) {
                    p = editor_line_end(ed, p);
                    if (p < ed->length)
                        ++p;
                    else
                        break;
                } else {
                    if (!s) {
                        p = 0;
                        break;
                    }
                    p = editor_line_start(ed, s - 1);
                }
            } while (p < ed->length && !editor_heading(ed, p));
        } else {
            snprintf(ed->message, sizeof(ed->message), "Unknown command: %.30s", m);
            *valid = false;
            break;
        }
    }
    return p;
}
static bool object(struct editor *ed, const char *m, unsigned count, size_t *a, size_t *b,
                   bool *linewise)
{
    bool around = m[0] == 'a';
    char c = m[1];
    size_t p = ed->cursor;
    *a = *b = p;
    *linewise = false;
    if (c == 'w' || c == 'W') {
        bool big = c == 'W';
        int kind = p < ed->length ? word((unsigned char)ed->text[p], big) : 0;
        while (*a && word((unsigned char)ed->text[editor_previous(ed, *a)], big) == kind)
            *a = editor_previous(ed, *a);
        while (*b < ed->length && word((unsigned char)ed->text[*b], big) == kind)
            *b = editor_next(ed, *b);
        while (--count && *b < ed->length) {
            while (*b < ed->length && !word((unsigned char)ed->text[*b], big))
                *b = editor_next(ed, *b);
            kind = *b < ed->length ? word((unsigned char)ed->text[*b], big) : 0;
            while (*b < ed->length && word((unsigned char)ed->text[*b], big) == kind)
                *b = editor_next(ed, *b);
        }
        if (around) {
            size_t end = *b;
            while (*b < ed->length && (ed->text[*b] == ' ' || ed->text[*b] == '\t'))
                ++*b;
            if (end == *b)
                while (*a && (ed->text[*a - 1] == ' ' || ed->text[*a - 1] == '\t'))
                    --*a;
        }
        return *a < *b;
    }
    if (c == 'p') {
        *a = editor_line_start(ed, p);
        while (*a &&
               editor_line_end(ed, editor_line_start(ed, *a - 1)) > editor_line_start(ed, *a - 1))
            *a = editor_line_start(ed, *a - 1);
        *b = editor_line_end(ed, p);
        while (*b < ed->length && editor_line_end(ed, *b + 1) > *b + 1)
            *b = editor_line_end(ed, *b + 1);
        if (*b < ed->length)
            ++*b;
        if (around)
            while (*b < ed->length && ed->text[*b] == '\n')
                ++*b;
        *linewise = true;
        return *a < *b;
    }
    if (c == 'b')
        c = '(';
    if (c == 'B')
        c = '{';
    const char *opens = "([{<", *closes = ")]}>";
    const char *q = strchr(opens, c);
    if (!q && strchr(closes, c)) {
        q = opens + (strchr(closes, c) - closes);
        c = *q;
    }
    if (q) {
        char close = closes[q - opens];
        int depth = 0;
        bool found = false;
        size_t start = p;
        for (;;) {
            if (ed->text[start] == c && depth == 0) {
                found = true;
                break;
            }
            if (ed->text[start] == close && start != p)
                ++depth;
            if (ed->text[start] == c)
                --depth;
            if (!start)
                break;
            --start;
        }
        if (!found)
            return false;
        depth = 1;
        size_t end = start + 1;
        while (end < ed->length) {
            if (ed->text[end] == c)
                ++depth;
            if (ed->text[end] == close && !--depth)
                break;
            ++end;
        }
        if (end == ed->length)
            return false;
        *a = start + !around;
        *b = end + around;
        return true;
    }
    if (strchr("\"'`", c)) {
        size_t start = editor_line_start(ed, p), end = editor_line_end(ed, p), left = SIZE_MAX;
        for (size_t i = start; i < end; ++i)
            if (ed->text[i] == c && (i == start || ed->text[i - 1] != '\\')) {
                if (left == SIZE_MAX)
                    left = i;
                else {
                    if (p <= i) {
                        *a = left + !around;
                        *b = i + around;
                        return true;
                    }
                    left = SIZE_MAX;
                }
            }
    }
    return false;
}
/* gq: paragraphs are joined and rewrapped at 79 columns, keeping their first line's indent. */
#define FORMAT_WIDTH 79
struct text_buffer {
    char *data;
    size_t length, capacity;
    bool failed;
};
static void append(struct text_buffer *out, const char *text, size_t n)
{
    if (out->failed)
        return;
    if (out->length + n + 1 > out->capacity) {
        size_t capacity = out->capacity ? out->capacity : 256;
        while (capacity < out->length + n + 1)
            capacity *= 2;
        char *data = realloc(out->data, capacity);
        if (!data) {
            out->failed = true;
            return;
        }
        out->data = data;
        out->capacity = capacity;
    }
    memcpy(out->data + out->length, text, n);
    out->length += n;
    out->data[out->length] = 0;
}
static size_t columns(const char *text, size_t n, size_t start)
{
    size_t c = start;
    for (size_t i = 0; i < n; ++i)
        if (text[i] == '\t')
            c += 4 - c % 4;
        else if (((unsigned char)text[i] & 0xc0) != 0x80)
            ++c;
    return c - start;
}
static bool blank(char c) { return c == ' ' || c == '\t'; }
/* A line holding only an image reference stays on its own line. */
static bool image_reference(const char *line, size_t n)
{
    size_t i = 0;
    while (i < n && blank(line[i]))
        ++i;
    while (n > i && blank(line[n - 1]))
        --n;
    if (n - i < 5 || line[i] != '!' || line[i + 1] != '[' || line[n - 1] != ')')
        return false;
    const char *close = memchr(line + i, ']', n - i);
    return close && close + 1 < line + n && close[1] == '(';
}
static void wrap(struct text_buffer *out, const char *indent, size_t indent_length,
                 const char *text, size_t n)
{
    size_t width = columns(indent, indent_length, 0);
    size_t limit = width < FORMAT_WIDTH ? FORMAT_WIDTH - width : 1;
    for (;;) {
        size_t cut = SIZE_MAX, cols = 0;
        for (size_t i = 0; i < n; ++i) {
            if (blank(text[i]) && cols <= limit)
                cut = i;
            if (blank(text[i]) && cols > limit && cut == SIZE_MAX)
                cut = i; /* A word longer than the width stays whole. */
            if (((unsigned char)text[i] & 0xc0) != 0x80)
                ++cols;
            if (cols > limit && cut != SIZE_MAX)
                break;
        }
        append(out, indent, indent_length);
        if (columns(text, n, width) <= limit || cut == SIZE_MAX) {
            append(out, text, n);
            return;
        }
        size_t end = cut;
        while (end && blank(text[end - 1]))
            --end;
        append(out, text, end);
        append(out, "\n", 1);
        while (cut < n && blank(text[cut]))
            ++cut;
        text += cut;
        n -= cut;
    }
}
static void format_lines(struct editor *ed, size_t a, size_t b)
{
    a = editor_line_start(ed, a);
    size_t last = editor_line_end(ed, b > a ? b - 1 : a);
    struct text_buffer out = {0}, paragraph = {0};
    const char *indent = NULL;
    size_t indent_length = 0;
    append(&out, "", 0);
    for (size_t p = a; p <= last;) {
        size_t end = editor_line_end(ed, p), start = p;
        while (start < end && blank(ed->text[start]))
            ++start;
        bool separator = start == end || image_reference(ed->text + p, end - p);
        if (separator || !paragraph.length) {
            if (paragraph.length) {
                wrap(&out, indent, indent_length, paragraph.data, paragraph.length);
                append(&out, "\n", 1);
                paragraph.length = 0;
            }
            if (separator) {
                append(&out, ed->text + p, end - p);
                append(&out, "\n", 1);
            } else {
                indent = ed->text + p;
                indent_length = start - p;
                append(&paragraph, ed->text + start, end - start);
            }
        } else {
            /* Like Vim's join, trailing blanks already separate the words. */
            if (!blank(paragraph.data[paragraph.length - 1]))
                append(&paragraph, " ", 1);
            append(&paragraph, ed->text + start, end - start);
        }
        if (end >= last)
            break;
        p = end + 1;
    }
    if (paragraph.length) {
        /* The indent points into the text, which is only replaced after wrapping. */
        wrap(&out, indent, indent_length, paragraph.data, paragraph.length);
        append(&out, "\n", 1);
    }
    free(paragraph.data);
    if (out.failed || paragraph.failed) {
        snprintf(ed->message, sizeof(ed->message), "Out of memory");
        free(out.data);
        return;
    }
    --out.length; /* The final line break belongs to the text after the range. */
    size_t last_start = out.length;
    while (last_start && out.data[last_start - 1] != '\n')
        --last_start;
    bool own = !editor_change_active(ed);
    editor_change_begin(ed);
    editor_replace(ed, a, last, out.data, out.length);
    ed->cursor = first_nonblank(ed, a + last_start);
    if (own)
        editor_change_end(ed);
    free(out.data);
}
static void operate(struct editor *ed, char op, size_t a, size_t b, bool linewise)
{
    a = min(a, ed->length);
    b = min(b, ed->length);
    if (a > b) {
        size_t tmp = a;
        a = b;
        b = tmp;
    }
    if (ed->mode == VISUAL || ed->mode == VISUAL_LINE) {
        ed->visual_first = a;
        ed->visual_last = b;
    }
    ed->mode = NORMAL;
    if (op == 'y') {
        editor_yank(ed, a, b, linewise, true);
        ed->cursor = a;
    } else if (op == 'd' || op == 'c') {
        editor_yank(ed, a, b, linewise, false);
        bool own = !editor_change_active(ed);
        editor_change_begin(ed);
        if (linewise && op == 'c') {
            if (b && ed->text[b - 1] == '\n')
                --b;
        } else if (linewise && b == ed->length && a && ed->text[a - 1] == '\n' &&
                   (a == b || ed->text[b - 1] != '\n'))
            --a;
        editor_replace(ed, a, b, "", 0);
        ed->cursor = a;
        if (op == 'c')
            ed->mode = INSERT;
        else if (own)
            editor_change_end(ed);
    } else if (op == '>' || op == '<')
        editor_indent(ed, a, b > a ? editor_previous(ed, b) : b, op == '>');
    else if (op == 'J')
        editor_join(ed, a, b > a ? editor_previous(ed, b) : b);
    else if (strchr("uU~", op))
        case_range(ed, a, b, op);
    else if (op == 'q')
        format_lines(ed, a, b);
    editor_clamp(ed);
}
static void prompt(struct editor *ed, char kind)
{
    ed->command_active = true;
    if (kind != ':') {
        ed->search_word = false;
        ed->search_hidden = false;
    }
    ed->command_kind = kind;
    ed->command_length = 0;
    ed->command[0] = 0;
    ed->search_origin = ed->cursor;
    ed->history_position = ed->history_count;
    if (kind == ':' && (ed->mode == VISUAL || ed->mode == VISUAL_LINE)) {
        size_t a, b;
        bool line;
        editor_selection(ed, &a, &b, &line);
        ed->visual_first = a;
        ed->visual_last = b;
        ed->mode = NORMAL;
        strcpy(ed->command, "'<,'>");
        ed->command_length = 5;
    }
}
static void command_input(struct editor *ed, enum editor_key key, const char *text)
{
    if (key == KEY_ESCAPE) {
        ed->command_active = false;
        ed->cursor = ed->search_origin;
        return;
    }
    if (key == KEY_ENTER) {
        ed->command_active = false;
        if (ed->command_length) {
            if (ed->history_count == 32) {
                free(ed->command_history[0]);
                memmove(ed->command_history, ed->command_history + 1, 31 * sizeof(char *));
                --ed->history_count;
            }
            char *entry = malloc(ed->command_length + 2);
            if (entry) {
                entry[0] = ed->command_kind;
                memcpy(entry + 1, ed->command, ed->command_length + 1);
                ed->command_history[ed->history_count++] = entry;
            }
        }
        if (ed->command_kind == ':')
            editor_command(ed, ed->command);
        else {
            ed->cursor = ed->search_origin;
            if (ed->command[0])
                snprintf(ed->search, sizeof(ed->search), "%.255s", ed->command);
            ed->search_forward = ed->command_kind == '/';
            editor_search(ed, ed->search_forward, 1);
            if (!ed->search_failed && ed->cursor != ed->search_origin)
                editor_jump_from(ed, ed->search_origin);
        }
        return;
    }
    if (key == KEY_UP || key == KEY_DOWN) {
        unsigned p = ed->history_position;
        while (key == KEY_UP ? p > 0 : p < ed->history_count) {
            if (key == KEY_UP)
                --p;
            else
                ++p;
            if (p == ed->history_count || ed->command_history[p][0] == ed->command_kind)
                break;
        }
        ed->history_position = p;
        snprintf(ed->command, sizeof(ed->command), "%s",
                 p < ed->history_count ? ed->command_history[p] + 1 : "");
        ed->command_length = strlen(ed->command);
    } else if (key == KEY_BACKSPACE) {
        if (!ed->command_length) {
            ed->command_active = false;
            ed->cursor = ed->search_origin;
            return;
        }
        do {
            --ed->command_length;
        } while (ed->command_length &&
                 ((unsigned char)ed->command[ed->command_length] & 0xc0) == 0x80);
        ed->command[ed->command_length] = 0;
    } else if (key == KEY_TEXT && text) {
        size_t n = strlen(text);
        if (ed->command_length + n < sizeof(ed->command)) {
            memcpy(ed->command + ed->command_length, text, n + 1);
            ed->command_length += n;
        }
    }
    if (ed->command_kind != ':' && ed->command[0]) {
        snprintf(ed->search, sizeof(ed->search), "%.255s", ed->command);
        ed->cursor = ed->search_origin;
        editor_search(ed, ed->command_kind == '/', 1);
    }
}
static void repeat(struct editor *ed, unsigned count);
static void replace_begin(struct editor *ed)
{
    size_t end = editor_line_end(ed, ed->cursor), n = end - ed->cursor;
    free(ed->replace_original);
    ed->replace_original = malloc(n + 1);
    if (ed->replace_original) {
        memcpy(ed->replace_original, ed->text + ed->cursor, n);
        ed->replace_original[n] = 0;
    }
    ed->replace_start = ed->cursor;
    ed->replacing = true;
}
static void replace_end(struct editor *ed)
{
    free(ed->replace_original);
    ed->replace_original = NULL;
    ed->replacing = false;
}
/* Type over one character per typed character; past the line's end, text is added. */
static void replace_text(struct editor *ed, const char *text)
{
    for (size_t i = 0, n = strlen(text); i < n;) {
        size_t next = i + 1;
        while (next < n && ((unsigned char)text[next] & 0xc0) == 0x80)
            ++next;
        size_t at = ed->cursor, end = at < editor_line_end(ed, at) ? editor_next(ed, at) : at;
        if (!editor_replace(ed, at, end, text + i, next - i))
            return;
        ed->cursor = at + next - i; /* Typing over an identical character leaves the text alone. */
        i = next;
    }
}
/* Backspace in replace mode puts back what was typed over and removes what was added. */
static void replace_backspace(struct editor *ed)
{
    size_t start = ed->replace_start, cursor = ed->cursor;
    if (cursor <= start || editor_line_start(ed, cursor) > start || !ed->replace_original) {
        if (cursor > editor_line_start(ed, cursor))
            ed->cursor = editor_previous(ed, cursor);
        return;
    }
    size_t previous = editor_previous(ed, cursor), typed = 0;
    for (size_t p = start; p < previous; p = editor_next(ed, p))
        ++typed;
    const char *original = ed->replace_original;
    size_t at = 0, length = strlen(original);
    for (; typed && at < length; --typed)
        do
            ++at;
        while (at < length && ((unsigned char)original[at] & 0xc0) == 0x80);
    size_t end = at;
    if (end < length)
        do
            ++end;
        while (end < length && ((unsigned char)original[end] & 0xc0) == 0x80);
    editor_replace(ed, previous, cursor, original + at, typed ? 0 : end - at);
    ed->cursor = previous;
}
static void input(struct editor *ed, enum editor_key key, const char *text)
{
    ed->message[0] = 0;
    if (ed->command_active) {
        command_input(ed, key, text);
        return;
    }
    if (ed->view == VIEW_READING &&
        (key == KEY_CUT || key == KEY_CTRL_V || key == KEY_DELETE || key == KEY_BACKSPACE ||
         key == KEY_TAB || key == KEY_SHIFT_TAB || key == KEY_CTRL_W)) {
        editor_can_edit(ed);
        return;
    }
    if (key == KEY_CTRL_S) {
        editor_save(ed, ed->filename);
        return;
    }
    if (key == KEY_CTRL_Q) {
        editor_command(ed, "q");
        return;
    }
    if (key == KEY_ZOOM_IN || key == KEY_ZOOM_OUT || key == KEY_ZOOM_RESET) {
        ed->requests |= key == KEY_ZOOM_IN    ? ED_ZOOM_IN
                        : key == KEY_ZOOM_OUT ? ED_ZOOM_OUT
                                              : ED_ZOOM_RESET;
        return;
    }
    if (key == KEY_COPY_IMAGE_TEXT) {
        ed->requests |= ED_COPY_OCR;
        return;
    }
    if (key == KEY_COPY || key == KEY_CUT) {
        size_t a, b;
        bool line;
        if (!editor_selection(ed, &a, &b, &line)) {
            a = editor_line_start(ed, ed->cursor);
            b = editor_line_end(ed, ed->cursor);
            if (b < ed->length)
                ++b;
            line = true;
        }
        operate(ed, key == KEY_COPY ? 'y' : 'd', a, b, line);
        return;
    }
    if (key == KEY_CTRL_V) {
        ed->put_after = true;
        ed->put_count = 1;
        ed->requests |= ED_PASTE;
        return;
    }
    if (key == KEY_ESCAPE || key == KEY_CTRL_C) {
        if (ed->mode == INSERT) {
            editor_change_end(ed);
            if (ed->cursor > editor_line_start(ed, ed->cursor))
                ed->cursor = editor_previous(ed, ed->cursor);
        }
        replace_end(ed);
        ed->mode = NORMAL;
        ed->pending[0] = 0;
        editor_clamp(ed);
        return;
    }
    if (key == KEY_CTRL_R) {
        editor_undo(ed, true);
        return;
    }
    if (ed->mode != INSERT && (key == KEY_CTRL_O || key == KEY_CTRL_I || key == KEY_TAB)) {
        ed->pending[0] = 0;
        jump_list(ed, key == KEY_CTRL_O);
        return;
    }
    if (key == KEY_CTRL_D || (key == KEY_CTRL_U && ed->mode != INSERT) || key == KEY_CTRL_F ||
        key == KEY_CTRL_B || key == KEY_PAGE_UP || key == KEY_PAGE_DOWN) {
        unsigned rows = ed->viewport_rows ? ed->viewport_rows : 24;
        if (key == KEY_CTRL_D || key == KEY_CTRL_U)
            rows = max(1, rows / 2);
        bool down = key == KEY_CTRL_D || key == KEY_CTRL_F || key == KEY_PAGE_DOWN;
        ed->cursor = vertical(ed, ed->cursor, down ? (long)rows : -(long)rows);
        editor_clamp(ed);
        return;
    }
    if (key == KEY_CTRL_E || key == KEY_CTRL_Y) {
        ed->top = editor_line_start(ed, vertical(ed, ed->top, key == KEY_CTRL_E ? 1 : -1));
        return;
    }
    if (ed->mode == INSERT) {
        if (ed->replacing && key == KEY_TEXT && text && *text)
            replace_text(ed, text);
        else if (ed->replacing && key == KEY_BACKSPACE)
            replace_backspace(ed);
        else if (key == KEY_TEXT && text && *text)
            editor_replace(ed, ed->cursor, ed->cursor, text, strlen(text));
        else if (key == KEY_BACKSPACE && ed->cursor)
            editor_replace(ed, editor_previous(ed, ed->cursor), ed->cursor, "", 0);
        else if (key == KEY_DELETE && ed->cursor < ed->length)
            editor_replace(ed, ed->cursor, editor_next(ed, ed->cursor), "", 0);
        else if (key == KEY_CTRL_W) {
            size_t p = ed->cursor;
            while (p && space(ed->text[p - 1]) && ed->text[p - 1] != '\n')
                --p;
            while (p && !space(ed->text[p - 1]))
                p = editor_previous(ed, p);
            editor_replace(ed, p, ed->cursor, "", 0);
        } else if (key == KEY_CTRL_U)
            editor_replace(ed, editor_line_start(ed, ed->cursor), ed->cursor, "", 0);
        else if (key == KEY_TAB) {
            editor_replace(ed, ed->cursor, ed->cursor, "    ", 4);
        } else if (key == KEY_SHIFT_TAB) {
            size_t p = ed->cursor, start = editor_line_start(ed, p), before = ed->length;
            editor_indent(ed, p, p, false);
            size_t removed = before - ed->length;
            ed->cursor = p - start > removed ? p - removed : start;
        } else if (key == KEY_ENTER) {
            size_t a = editor_line_start(ed, ed->cursor), b = first_nonblank(ed, ed->cursor),
                   n = min(b, ed->cursor) - a;
            char *indent = malloc(n + 1);
            if (indent) {
                indent[0] = '\n';
                memcpy(indent + 1, ed->text + a, n);
                editor_replace(ed, ed->cursor, ed->cursor, indent, n + 1);
                free(indent);
            }
        }
    }
    const char *arrow = key == KEY_LEFT    ? "h"
                        : key == KEY_RIGHT ? "l"
                        : key == KEY_DOWN  ? "j"
                        : key == KEY_UP    ? "k"
                        : key == KEY_HOME  ? "0"
                        : key == KEY_END   ? "$"
                                           : NULL;
    if (ed->mode == INSERT) {
        if (key == KEY_END)
            ed->cursor = editor_line_end(ed, ed->cursor);
        else if (arrow) {
            bool l, i, v;
            ed->cursor = motion(ed, arrow, 1, false, &l, &i, &v);
        }
        return;
    }
    if (arrow)
        text = arrow;
    else if (key != KEY_TEXT || !text || !*text)
        return;
    size_t len = strlen(ed->pending), n = strlen(text);
    if (len + n >= sizeof(ed->pending)) {
        ed->pending[0] = 0;
        return;
    }
    memcpy(ed->pending + len, text, n + 1);
    const char *p = ed->pending;
    unsigned count = 0;
    while (*p >= '0' && *p <= '9' && (*p != '0' || count)) {
        count = min(100000, count * 10 + (unsigned)(*p++ - '0'));
    }
    if (!*p)
        return;
    char name = 0;
    if (*p == '"') {
        if (!p[1])
            return;
        name = p[1];
        if (!register_supported(name)) {
            snprintf(ed->message, sizeof(ed->message), "Register %.4s is not supported", p + 1);
            ed->pending[0] = 0;
            return;
        }
        p += 2;
        unsigned more = 0;
        while (*p >= '0' && *p <= '9' && (*p != '0' || more))
            more = min(100000, more * 10 + (unsigned)(*p++ - '0'));
        if (more)
            count = !count ? more : count > 100000 / more ? 100000 : count * more;
        if (!*p)
            return;
    }
    ed->register_name = name;
    bool explicit_count = count != 0;
    if (!count)
        count = 1;
    bool visual = ed->mode == VISUAL || ed->mode == VISUAL_LINE;
    if (ed->view == VIEW_READING &&
        ((!visual && (strchr("dc><iaIAoOsSxXDCJRr~pPu", *p) ||
                      (p[0] == 'g' && p[1] && strchr("uU~q", p[1])))) ||
         (visual && (strchr("dc><JuU~xspP", *p) || !strcmp(p, "gq"))))) {
        editor_can_edit(ed);
        ed->pending[0] = 0;
        return;
    }
    char op = 0;
    const char *op_keys = p;
    size_t op_length = 0;
    if (!visual && (strchr("dyc><", *p) || (p[0] == 'g' && p[1] && strchr("uU~q", p[1])))) {
        op_length = *p == 'g' ? 2 : 1;
        op = p[op_length - 1];
        p += op_length;
        unsigned inner = 0;
        while (isdigit((unsigned char)*p) && (*p != '0' || inner))
            inner = min(100000, inner * 10 + (unsigned)(*p++ - '0'));
        if (inner)
            count = count > 100000 / inner ? 100000 : count * inner;
        if (!*p)
            return;
    }
    if ((strchr("gz[]fFtTr'`", *p) || (!op && strchr("qZ@m", *p)) ||
         (op && (*p == 'i' || *p == 'a'))) &&
        !p[1])
        return;
    if (visual && (*p == 'i' || *p == 'a') && !p[1])
        return;
    char seq[40], keys_op[3] = {0};
    snprintf(seq, sizeof(seq), "%s", p);
    memcpy(keys_op, op_keys, op_length);
    ed->pending[0] = 0;
    p = seq;
    if (!op && (*p == 'q' || *p == '@')) {
        snprintf(ed->message, sizeof(ed->message), "Macros are not supported");
        return;
    }
    /* On a selected image, + and - change its width by 10% per count and = resets it. */
    if (!op && !visual && !p[1] && strchr("+-=", *p) && ed->image_resize &&
        ed->view != VIEW_SOURCE) {
        char spec[16] = "";
        if (*p != '=')
            snprintf(spec, sizeof(spec), "%c%u%%", *p, (unsigned)min(1000, count * 10));
        if (ed->image_resize(ed, ed->cursor, spec, ed->image_resize_data))
            return;
    }
    if (!op && *p == 'Z') {
        if (p[1] == 'Z' || p[1] == 'Q')
            editor_command(ed, p[1] == 'Z' ? "x" : "q!");
        else
            snprintf(ed->message, sizeof(ed->message), "Unknown command: %.30s", p);
        return;
    }
    if (!op && !strcmp(p, ":")) {
        prompt(ed, ':');
        return;
    }
    if (!op && (*p == '/' || *p == '?')) {
        prompt(ed, *p);
        return;
    }
    if (!op && (*p == '*' || *p == '#')) {
        size_t a = ed->cursor, b = a;
        int kind = a < ed->length ? word((unsigned char)ed->text[a], false) : 0;
        while (a && word((unsigned char)ed->text[editor_previous(ed, a)], false) == kind)
            a = editor_previous(ed, a);
        while (b < ed->length && word((unsigned char)ed->text[b], false) == kind)
            b = editor_next(ed, b);
        size_t bytes = min(b - a, sizeof(ed->search) - 1);
        memcpy(ed->search, ed->text + a, bytes);
        ed->search[bytes] = 0;
        ed->search_word = true;
        ed->search_forward = *p == '*';
        size_t origin = ed->cursor;
        editor_search(ed, ed->search_forward, count);
        if (ed->cursor != origin)
            editor_jump_from(ed, origin);
        return;
    }
    if (!op && (!strcmp(p, "v") || !strcmp(p, "V"))) {
        enum editor_mode target = *p == 'v' ? VISUAL : VISUAL_LINE;
        if (ed->mode == target)
            ed->mode = NORMAL;
        else {
            if (!visual)
                ed->anchor = ed->cursor;
            ed->mode = target;
        }
        return;
    }
    size_t a, b;
    bool linewise;
    if (visual && (!strcmp(p, "o"))) {
        size_t tmp = ed->cursor;
        ed->cursor = ed->anchor;
        ed->anchor = tmp;
        return;
    }
    if (visual && ((*p == 'i' || *p == 'a') && p[1])) {
        if (object(ed, p, count, &a, &b, &linewise)) {
            ed->anchor = a;
            ed->cursor = b > a ? editor_previous(ed, b) : a;
            if (linewise)
                ed->mode = VISUAL_LINE;
        }
        return;
    }
    if (visual && !strcmp(p, "gq")) {
        editor_selection(ed, &a, &b, &linewise);
        operate(ed, 'q', a, b, true);
        return;
    }
    if (visual && strchr("dyc><JuU~xspP", *p)) {
        editor_selection(ed, &a, &b, &linewise);
        if (*p == 'p' || *p == 'P') {
            put(ed, false, count);
            return;
        }
        operate(ed, *p == 'x' ? 'd' : *p == 's' ? 'c' : *p, a, b, linewise);
        return;
    }
    if (op && (!strcmp(p, op_length == 1 ? keys_op : keys_op + 1) || !strcmp(p, keys_op))) {
        a = editor_line_start(ed, ed->cursor);
        b = editor_line_end(ed, vertical(ed, a, count - 1));
        if (b < ed->length)
            ++b;
        operate(ed, op, a, b, true);
        return;
    }
    if (op && (*p == 'i' || *p == 'a')) {
        if (object(ed, p, count, &a, &b, &linewise))
            operate(ed, op, a, b, linewise);
        return;
    }
    if (!op && !strcmp(p, "u")) {
        editor_undo(ed, false);
        return;
    }
    if (!op && *p == 'm') {
        if (p[1] >= 'a' && p[1] <= 'z')
            ed->marks[p[1] - 'a'] = ed->cursor + 1;
        else
            snprintf(ed->message, sizeof(ed->message), "Mark %.4s is not supported", p + 1);
        return;
    }
    if (!op && !strcmp(p, ".")) {
        repeat(ed, explicit_count ? count : 0);
        return;
    }
    if (!op && (*p == 'p' || *p == 'P')) {
        put(ed, *p == 'p', count);
        return;
    }
    if (!op && (!strcmp(p, "gx"))) {
        ed->requests |= ED_OPEN;
        return;
    }
    if (!op && p[0] == 'z') {
        if (!strchr("ztb", p[1])) {
            snprintf(ed->message, sizeof(ed->message), "Unknown command: %.30s", p);
            return;
        }
        size_t line = editor_line_number(ed, ed->cursor),
               rows = ed->viewport_rows ? ed->viewport_rows : 24;
        size_t back = p[1] == 'z' ? rows / 2 : p[1] == 'b' ? rows - 1 : 0;
        ed->top = editor_line_at(ed, line > back ? line - back : 0);
        return;
    }
    if (!op && !strcmp(p, "R")) {
        editor_change_begin(ed);
        replace_begin(ed);
        ed->mode = INSERT;
        return;
    }
    if (!op && strchr("iaIAoOsS", *p)) {
        editor_change_begin(ed);
        if (*p == 'a' && ed->cursor < editor_line_end(ed, ed->cursor))
            ed->cursor = editor_next(ed, ed->cursor);
        if (*p == 'I')
            ed->cursor = first_nonblank(ed, ed->cursor);
        if (*p == 'A')
            ed->cursor = editor_line_end(ed, ed->cursor);
        if (*p == 'o' || *p == 'O') {
            size_t start = editor_line_start(ed, ed->cursor),
                   indent = first_nonblank(ed, start) - start;
            char *s = malloc(indent + 1);
            if (s) {
                if (*p == 'o') {
                    memcpy(s + 1, ed->text + start, indent);
                    s[0] = '\n';
                    ed->cursor = editor_line_end(ed, ed->cursor);
                } else {
                    memcpy(s, ed->text + start, indent);
                    s[indent] = '\n';
                    ed->cursor = start;
                }
                size_t at = ed->cursor;
                editor_replace(ed, at, at, s, indent + 1);
                if (*p == 'O')
                    ed->cursor = at + indent;
                free(s);
            }
        }
        if (*p == 's' || *p == 'S') {
            a = *p == 'S' ? editor_line_start(ed, ed->cursor) : ed->cursor;
            b = a;
            if (*p == 'S')
                b = editor_line_end(ed, a);
            else
                for (unsigned i = 0; i < count && b < editor_line_end(ed, a); ++i)
                    b = editor_next(ed, b);
            editor_yank(ed, a, b, false, false);
            editor_replace(ed, a, b, "", 0);
        }
        ed->mode = INSERT;
        return;
    }
    if (!op && strchr("xXDCYJr~", *p)) {
        a = ed->cursor;
        b = a;
        if (*p == 'D' || *p == 'C' || *p == 'Y') {
            b = editor_line_end(ed, vertical(ed, a, count - 1));
            if (*p == 'Y') {
                a = editor_line_start(ed, a);
                if (b < ed->length)
                    ++b;
            }
        } else if (*p == 'J') {
            b = editor_line_end(ed, vertical(ed, a, max(2, count) - 1));
            editor_join(ed, a, b);
            return;
        } else if (*p == 'X') {
            while (count-- && a > editor_line_start(ed, a))
                a = editor_previous(ed, a);
        } else {
            while (count-- && b < editor_line_end(ed, a))
                b = editor_next(ed, b);
        }
        if (*p == '~') {
            case_range(ed, a, b, '~');
            ed->cursor = b;
            editor_clamp(ed);
        } else if (*p == 'r' && p[1]) {
            editor_change_begin(ed);
            size_t off = 1;
            editor_codepoint(p, strlen(p), &off);
            while (a < b) {
                size_t end = editor_next(ed, a), old = end - a;
                editor_replace(ed, a, end, p + 1, off - 1);
                b = b - old + off - 1;
                a += off - 1;
            }
            ed->cursor = a ? editor_previous(ed, a) : a;
            editor_change_end(ed);
        } else
            operate(ed, *p == 'C' ? 'c' : *p == 'Y' ? 'y' : 'd', a, b, *p == 'Y');
        return;
    }
    bool inclusive, valid;
    size_t target = motion(ed, p, count, explicit_count, &linewise, &inclusive, &valid);
    if (!valid)
        return;
    if (op) {
        a = min(ed->cursor, target);
        b = max(ed->cursor, target);
        if (linewise) {
            a = editor_line_start(ed, a);
            b = editor_line_end(ed, b);
            if (b < ed->length)
                ++b;
        } else if (inclusive)
            b = editor_next(ed, b);
        /* Like Vim, an operator whose last word ends a line stops there instead of reaching
         * into the next line (dw on a line's last word keeps the line break). */
        if ((*p == 'w' || *p == 'W') && !linewise && b > a &&
            editor_line_number(ed, b) > editor_line_number(ed, a) &&
            b <= first_nonblank(ed, b))
            b = editor_line_start(ed, b) - 1;
        /* cw changes the current word, leaving its following whitespace. */
        if (op == 'c' && (*p == 'w' || *p == 'W'))
            while (b > a && space(ed->text[b - 1]))
                --b;
        operate(ed, op, a, b, linewise);
    } else {
        size_t origin = ed->cursor;
        ed->cursor = target;
        editor_clamp(ed);
        if (jump_motion(p) && ed->cursor != origin)
            editor_jump_from(ed, origin);
    }
}

/* Dot repeat records each event as a key byte, a 4-byte length and the text. */
enum { DOT_IDLE, DOT_COMMAND, DOT_INSERT, DOT_DISCARD };
#define DOT_LIMIT (1024 * 1024)
static void record(struct editor *ed, enum editor_key key, const char *text)
{
    size_t n = text ? strlen(text) : 0, need = ed->dot_record_length + 5 + n;
    if (need > DOT_LIMIT) {
        ed->dot_state = DOT_DISCARD;
        return;
    }
    if (need > ed->dot_record_capacity) {
        size_t capacity = ed->dot_record_capacity ? ed->dot_record_capacity : 64;
        while (capacity < need)
            capacity *= 2;
        unsigned char *data = realloc(ed->dot_record, capacity);
        if (!data) {
            ed->dot_state = DOT_DISCARD;
            return;
        }
        ed->dot_record = data;
        ed->dot_record_capacity = capacity;
    }
    unsigned char *at = ed->dot_record + ed->dot_record_length;
    at[0] = (unsigned char)key;
    for (unsigned i = 0; i < 4; ++i)
        at[1 + i] = (unsigned char)(n >> (8 * i));
    memcpy(at + 5, text, n);
    ed->dot_record_length = need;
}
static size_t event(const unsigned char *data, size_t at, enum editor_key *key, const char **text,
                    size_t *n)
{
    *key = (enum editor_key)data[at];
    *n = 0;
    for (unsigned i = 0; i < 4; ++i)
        *n |= (size_t)data[at + 1 + i] << (8 * i);
    *text = (const char *)data + at + 5;
    return at + 5 + *n;
}
/* The recorded command's typed text, without its count or register, such as "dw" or "u". */
static void recorded_command(const struct editor *ed, char *out, size_t size)
{
    size_t used = 0;
    for (size_t at = 0; at < ed->dot_record_length && used + 1 < size;) {
        enum editor_key key;
        const char *text;
        size_t n;
        at = event(ed->dot_record, at, &key, &text, &n);
        if (key != KEY_TEXT || used + n + 1 > size) {
            if (!used)
                out[used++] = 1; /* A non-text key started it: never a change to repeat. */
            break;
        }
        memcpy(out + used, text, n);
        used += n;
    }
    out[used] = 0;
    char *p = out;
    for (bool skipped = true; skipped;) {
        skipped = false;
        while (*p >= '1' && *p <= '9') {
            while (isdigit((unsigned char)*p))
                ++p;
            skipped = true;
        }
        if (p[0] == '"' && p[1]) {
            p += 2;
            skipped = true;
        }
    }
    memmove(out, p, strlen(p) + 1);
}
static void finish_record(struct editor *ed, bool keep)
{
    unsigned char *copy;
    if (keep && ed->dot_state != DOT_DISCARD && (copy = malloc(ed->dot_record_length))) {
        memcpy(copy, ed->dot_record, ed->dot_record_length);
        free(ed->dot);
        ed->dot = copy;
        ed->dot_length = ed->dot_record_length;
    }
    ed->dot_record_length = 0;
    ed->dot_state = DOT_IDLE;
}
static void after_input(struct editor *ed)
{
    if (ed->dot_state == DOT_INSERT || ed->dot_state == DOT_DISCARD) {
        if (ed->mode != INSERT)
            finish_record(ed, ed->dot_state == DOT_INSERT);
        return;
    }
    if (ed->dot_state != DOT_COMMAND || ed->pending[0])
        return;
    char command[64];
    recorded_command(ed, command, sizeof(command));
    bool repeatable = command[0] && command[0] != 1 && !strchr("u.:/?", command[0]);
    if (ed->command_active || !repeatable)
        finish_record(ed, false);
    else if (ed->mode == INSERT)
        ed->dot_state = DOT_INSERT;
    else
        finish_record(ed, ed->revision != ed->dot_revision || (ed->requests & ED_PASTE));
}
/* Replays the last change; a count replaces the one it was typed with. */
static void repeat(struct editor *ed, unsigned count)
{
    if (!ed->dot_length || ed->dot_replaying)
        return;
    size_t length = ed->dot_length;
    unsigned char *keys = malloc(length + 16);
    if (!keys)
        return;
    size_t used = 0, at = 0;
    if (count) {
        char digits[12];
        int n = snprintf(digits, sizeof(digits), "%u", count);
        /* Drop the recorded leading count, then prefix the new one. */
        while (at < length) {
            enum editor_key key;
            const char *text;
            size_t size;
            size_t next = event(ed->dot, at, &key, &text, &size);
            if (key != KEY_TEXT || size != 1 || !isdigit((unsigned char)*text) ||
                (at == 0 && *text == '0'))
                break;
            at = next;
        }
        keys[used] = KEY_TEXT;
        keys[used + 1] = (unsigned char)n;
        keys[used + 2] = keys[used + 3] = keys[used + 4] = 0;
        memcpy(keys + used + 5, digits, (size_t)n);
        used += 5 + (size_t)n;
    }
    memcpy(keys + used, ed->dot + at, length - at);
    used += length - at;
    if (count) {
        /* Later repeats use the new count too, as in Vim. */
        unsigned char *copy = malloc(used);
        if (copy) {
            memcpy(copy, keys, used);
            free(ed->dot);
            ed->dot = copy;
            ed->dot_length = used;
        }
    }
    ed->dot_replaying = true;
    for (size_t i = 0; i < used;) {
        enum editor_key key;
        const char *text;
        size_t n;
        size_t next = event(keys, i, &key, &text, &n);
        char buffer[64];
        char *value = n < sizeof(buffer) ? buffer : malloc(n + 1);
        if (!value)
            break;
        memcpy(value, text, n);
        value[n] = 0;
        input(ed, key, value);
        if (value != buffer)
            free(value);
        i = next;
    }
    /* Leave insert mode if the recording was cut short. */
    if (ed->mode == INSERT)
        input(ed, KEY_ESCAPE, "");
    ed->dot_replaying = false;
    free(keys);
}

void editor_input(struct editor *ed, enum editor_key key, const char *text)
{
    bool inserting = ed->mode == INSERT;
    if (ed->replacing && !inserting)
        replace_end(ed); /* Insert mode was left some other way, such as a click. */
    if (!ed->dot_replaying && ed->dot_state == DOT_IDLE && ed->mode == NORMAL &&
        !ed->command_active && !ed->pending[0]) {
        ed->dot_state = DOT_COMMAND;
        ed->dot_record_length = 0;
        ed->dot_revision = ed->revision;
    }
    if (ed->dot_state != DOT_IDLE && ed->dot_state != DOT_DISCARD)
        record(ed, key, text);
    ed->register_name = 0;
    input(ed, key, text);
    ed->register_name = 0;
    after_input(ed);
    if (ed->replacing && ed->mode != INSERT)
        replace_end(ed);
    if (inserting || ed->mode == INSERT)
        ed->search_hidden = true;
}
