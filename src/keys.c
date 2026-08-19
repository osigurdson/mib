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
void editor_yank(struct editor *ed, size_t a, size_t b, bool linewise)
{
    if (b <= a)
        return;
    char *copy = malloc(b - a + 2);
    if (!copy)
        return;
    memcpy(copy, ed->text + a, b - a);
    size_t n = b - a;
    if (linewise && copy[n - 1] != '\n')
        copy[n++] = '\n';
    copy[n] = 0;
    free(ed->register_text);
    ed->register_text = copy;
    ed->register_length = n;
    ed->register_linewise = linewise;
    ed->requests |= ED_COPY;
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
        editor_yank(ed, a, b, linewise);
        ed->cursor = a;
    } else if (op == 'd' || op == 'c') {
        editor_yank(ed, a, b, linewise);
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
static void input(struct editor *ed, enum editor_key key, const char *text)
{
    ed->message[0] = 0;
    if (ed->command_active) {
        command_input(ed, key, text);
        return;
    }
    if (ed->render_mode == RENDER_POLISH &&
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
        ed->mode = NORMAL;
        ed->pending[0] = 0;
        editor_clamp(ed);
        return;
    }
    if (key == KEY_CTRL_R) {
        editor_undo(ed, true);
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
        if (key == KEY_TEXT && text && *text)
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
    bool explicit_count = count != 0;
    if (!count)
        count = 1;
    bool visual = ed->mode == VISUAL || ed->mode == VISUAL_LINE;
    if (ed->render_mode == RENDER_POLISH &&
        ((!visual && (strchr("dc><iaIAoOsSxXDCJr~pPu", *p) ||
                      (p[0] == 'g' && p[1] && strchr("uU~", p[1])))) ||
         (visual && strchr("dc><JuU~xspP", *p)))) {
        editor_can_edit(ed);
        ed->pending[0] = 0;
        return;
    }
    char op = 0;
    const char *op_keys = p;
    size_t op_length = 0;
    if (!visual && (strchr("dyc><", *p) || (p[0] == 'g' && p[1] && strchr("uU~", p[1])))) {
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
        editor_search(ed, ed->search_forward, count);
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
    if (visual && strchr("dyc><JuU~xspP", *p)) {
        editor_selection(ed, &a, &b, &linewise);
        if (*p == 'p' || *p == 'P') {
            ed->put_after = false;
            ed->put_count = count;
            ed->requests |= ED_PASTE;
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
    if (!op && (*p == 'p' || *p == 'P')) {
        ed->put_after = *p == 'p';
        ed->put_count = count;
        ed->requests |= ED_PASTE;
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
            editor_yank(ed, a, b, false);
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
        /* cw changes the current word, leaving its following whitespace. */
        if (op == 'c' && (*p == 'w' || *p == 'W'))
            while (b > a && space(ed->text[b - 1]))
                --b;
        operate(ed, op, a, b, linewise);
    } else {
        ed->cursor = target;
        editor_clamp(ed);
    }
}

void editor_input(struct editor *ed, enum editor_key key, const char *text)
{
    bool inserting = ed->mode == INSERT;
    input(ed, key, text);
    if (inserting || ed->mode == INSERT)
        ed->search_hidden = true;
}
