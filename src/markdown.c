#include "markdown.h"
#include <ctype.h>
#include <string.h>
static bool boundary(unsigned char c) { return !isalnum(c) && c != '_' && c < 128; }
static void add(struct md_span *out, size_t *n, size_t capacity, size_t a, size_t b, unsigned flags)
{
    if (a < b && *n < capacity)
        out[(*n)++] = (struct md_span){a, b, flags};
}
/* Only bounded metadata is retained, never a document tree. */
struct md_block markdown_block(const char *s, size_t start, size_t end)
{
    struct md_block b = {0};
    size_t p = start;
    unsigned spaces = 0;
    while (p < end && (s[p] == ' ' || s[p] == '\t')) {
        spaces += s[p] == '\t' ? 4 - spaces % 4 : 1;
        ++p;
    }
    if (p < end && strchr("-*_", s[p])) {
        char marker = s[p];
        unsigned count = 0;
        bool valid = true;
        for (size_t q = p; q < end; ++q) {
            if (s[q] == marker) ++count;
            else if (s[q] != ' ' && s[q] != '\t') valid = false;
        }
        b.rule = valid && count >= 3;
    }
    size_t q = p;
    while (q < end && s[q] == '#') ++q;
    if (q > p && q - p <= 6 && q < end && s[q] == ' ')
        b.heading = (unsigned)(q - p);
    q = p;
    if (q < end && strchr("-+*", s[q])) ++q;
    else {
        while (q < end && q - p < 9 && s[q] >= '0' && s[q] <= '9') ++q;
        if (q == p || q == end || (s[q] != '.' && s[q] != ')')) q = p;
        else ++q;
    }
    if (!b.rule && q > p && q < end && s[q] == ' ') {
        b.indent = spaces + (unsigned)(q - p) + 1;
        if (q + 4 < end && s[q + 1] == '[' && s[q + 3] == ']' &&
            strchr(" xX", s[q + 2]) && s[q + 4] == ' ')
            b.indent = spaces + 2; /* concealed task marker occupies one cell */
    }
    return b;
}
static void inlines(const char *s, size_t start, size_t end, struct md_span *out,
                    size_t *n, size_t capacity, unsigned depth)
{
    for (size_t i = start; i < end; ++i) {
        if (capacity - *n < 8) break; /* Leave excess constructs literal as a unit. */
        if (s[i] == '\\') {
            if (i + 1 < end)
                add(out, n, capacity, i, i + 1, MD_HIDE);
            ++i;
            continue;
        }
        if (s[i] == '`') {
            size_t run = 1;
            while (i + run < end && s[i + run] == '`')
                ++run;
            size_t q = i + run;
            while (q + run <= end && memcmp(s + i, s + q, run))
                ++q;
            if (q + run <= end) {
                add(out, n, capacity, i, i + run, MD_DIM | MD_HIDE);
                add(out, n, capacity, i + run, q, MD_CODE);
                add(out, n, capacity, q, q + run, MD_DIM | MD_HIDE);
                i = q + run - 1;
            }
            continue;
        }
        if ((i == start || boundary((unsigned char)s[i - 1])) &&
            ((end - i >= 8 && !memcmp(s + i, "https://", 8)) ||
             (end - i >= 7 && !memcmp(s + i, "http://", 7)))) {
            size_t q = i;
            while (q < end && !isspace((unsigned char)s[q]) && s[q] != '<' && s[q] != '>')
                ++q;
            while (q > i && strchr(".,;:!?", s[q - 1])) --q;
            add(out, n, capacity, i, q, MD_LINK);
            if (q > i) i = q - 1;
            continue;
        }
        if (s[i] == '[' || (s[i] == '!' && i + 1 < end && s[i + 1] == '[')) {
            size_t a = i + (s[i] == '!'), q = a + 1;
            while (q < end && s[q] != ']')
                ++q;
            if (q + 1 < end && s[q + 1] == '(') {
                size_t r = q + 2;
                while (r < end && s[r] != ')')
                    ++r;
                if (r < end) {
                    unsigned conceal = s[i] == '!' ? 0 : MD_HIDE;
                    add(out, n, capacity, i, a + 1, MD_DIM | conceal);
                    add(out, n, capacity, a + 1, q, MD_LINK);
                    add(out, n, capacity, q, r + 1, MD_DIM | conceal);
                    if (depth < 8)
                        inlines(s, a + 1, q, out, n, capacity, depth + 1);
                    i = r;
                }
            }
            continue;
        }
        if (s[i] == '*' || s[i] == '_' || (s[i] == '~' && i + 1 < end && s[i + 1] == '~')) {
            char c = s[i];
            size_t run = 1;
            while (run < 3 && i + run < end && s[i + run] == c)
                ++run;
            if (c == '~') run = 2;
            if ((i > start && !boundary((unsigned char)s[i - 1])) || i + run >= end ||
                isspace((unsigned char)s[i + run]))
                continue;
            size_t q = i + run;
            while (q + run <= end) {
                if (s[q] == '\\' && q + 1 < end) { q += 2; continue; }
                if (s[q] == '`') {
                    size_t ticks = 1, r;
                    while (q + ticks < end && s[q + ticks] == '`') ++ticks;
                    for (r = q + ticks; r + ticks <= end; ++r)
                        if (!memcmp(s + q, s + r, ticks)) break;
                    if (r + ticks <= end) { q = r + ticks; continue; }
                }
                size_t count = 0;
                while (q + count < end && s[q + count] == c) ++count;
                if (count >= run && (count == run || count == 3) && q > i + run &&
                    !isspace((unsigned char)s[q - 1]) &&
                    (q + count == end || boundary((unsigned char)s[q + count]))) {
                    q += count - run;
                    break;
                }
                q += count ? count : 1;
            }
            if (q + run <= end) {
                add(out, n, capacity, i, i + run, MD_DIM | MD_HIDE);
                add(out, n, capacity, i + run, q, c == '~' ? MD_STRIKE : run == 3 ? MD_BOLD | MD_ITALIC : run == 2 ? MD_BOLD : MD_ITALIC);
                add(out, n, capacity, q, q + run, MD_DIM | MD_HIDE);
                if (depth < 8)
                    inlines(s, i + run, q, out, n, capacity, depth + 1);
                i = q + run - 1;
            }
        }
    }
}

size_t markdown_line(const char *s, size_t start, size_t end, struct md_span *out, size_t capacity,
                     bool fenced)
{
    size_t n = 0, p = start;
    while (p < end && (s[p] == ' ' || s[p] == '\t'))
        ++p;
    struct md_block block = markdown_block(s, start, end);
    if (fenced) {
        add(out, &n, capacity, start, end, MD_CODE);
        return n;
    }
    if (block.rule) {
        add(out, &n, capacity, start, end, MD_DIM);
        return n;
    }
    if (p < end && s[p] == '#') {
        size_t q = p;
        while (q < end && s[q] == '#')
            ++q;
        if (q - p <= 6 && q < end && s[q] == ' ') {
            add(out, &n, capacity, p, q, MD_DIM | MD_HIDE);
            add(out, &n, capacity, q, q + 1, MD_HIDE);
            add(out, &n, capacity, q, end, MD_HEADING | MD_BOLD);
        }
    }
    if (p < end && s[p] == '>') {
        add(out, &n, capacity, p, p + 1, MD_DIM | MD_HIDE);
        if (p + 1 < end && s[p + 1] == ' ')
            add(out, &n, capacity, p + 1, p + 2, MD_HIDE);
        add(out, &n, capacity, p + 1, end, MD_QUOTE);
    }
    if (block.indent && p < end && s[p] >= '0' && s[p] <= '9') {
        size_t q = p;
        while (q < end && s[q] != ' ') ++q;
        add(out, &n, capacity, p, q, MD_DIM);
    }
    if (p + 1 < end && strchr("-*+", s[p]) && s[p + 1] == ' ') {
        bool task = p + 5 < end && s[p + 2] == '[' && s[p + 4] == ']' && s[p + 5] == ' ' && strchr(" xX", s[p + 3]);
        add(out, &n, capacity, p, p + 1, MD_DIM | (task ? MD_HIDE : MD_BULLET));
        if (task) {
            bool checked = s[p + 3] == 'x' || s[p + 3] == 'X';
            add(out, &n, capacity, p + 1, p + 2, MD_HIDE);
            add(out, &n, capacity, p + 2, p + 5, MD_DIM);
            add(out, &n, capacity, p + 2, p + 3, MD_TASK | (checked ? MD_CHECKED : 0));
            add(out, &n, capacity, p + 3, p + 5, MD_HIDE);
            if (checked)
                add(out, &n, capacity, p + 5, end, MD_DONE);
        }
    }
    inlines(s, start, end, out, &n, capacity, 0);
    return n;
}

/* Pipe tables deliberately have a small, fixed column limit. Escaped pipes
 * remain inside their cell, including in inline code. */
unsigned markdown_cells(const char *s, size_t start, size_t end, struct md_cell *cells)
{
    while (start < end && (s[start] == ' ' || s[start] == '\t')) ++start;
    while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t')) --end;
    bool pipe = false;
    if (start < end && s[start] == '|') { ++start; pipe = true; }
    unsigned count = 0;
    size_t a = start;
    for (size_t p = start; p <= end; ++p) {
        if (p < end && s[p] == '\\' && p + 1 < end) { ++p; continue; }
        if (p != end && s[p] != '|') continue;
        if (count == MD_TABLE_COLUMNS) return 0;
        size_t b = p;
        while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
        while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
        cells[count++] = (struct md_cell){a, b};
        if (p < end) pipe = true;
        if (p + 1 == end && s[p] == '|') break;
        a = p + 1;
    }
    return pipe ? count : 0;
}
static bool table_separator(const char *s, size_t a, size_t b, unsigned count,
                            unsigned char *align)
{
    struct md_cell cells[MD_TABLE_COLUMNS];
    unsigned char found[MD_TABLE_COLUMNS];
    if (!count || markdown_cells(s, a, b, cells) != count) return false;
    for (unsigned i = 0; i < count; ++i) {
        size_t p = cells[i].start, end = cells[i].end;
        bool left = p < end && s[p] == ':';
        if (left) ++p;
        size_t dashes = p;
        while (p < end && s[p] == '-') ++p;
        if (p - dashes < 3) return false;
        bool right = p < end && s[p] == ':';
        if (right) ++p;
        if (p != end) return false;
        found[i] = right ? (left ? 1 : 2) : 0;
    }
    memcpy(align, found, count);
    return true;
}
void markdown_table(const char *s, size_t length, size_t start, size_t end,
                    struct md_table *table, bool continuation)
{
    unsigned previous = table->count;
    table->count = markdown_cells(s, start, end, table->cells);
    table->header = table->separator = false;
    if (!table->count) return;
    if (continuation && table->count == previous) {
        table->separator = table_separator(s, start, end, table->count, table->align);
        return;
    }
    size_t next = end < length ? end + 1 : end, next_end = next;
    while (next_end < length && s[next_end] != '\n') ++next_end;
    if (table_separator(s, next, next_end, table->count, table->align)) {
        table->header = true;
        return;
    }
    if (continuation && !previous) { table->count = 0; return; }
    /* Random access (scrolling/clicking) reconstructs context without an AST. */
    size_t a = start, b = end;
    struct md_cell cells[MD_TABLE_COLUMNS];
    for (;;) {
        if (table_separator(s, a, b, table->count, table->align) && a) {
            size_t header_end = a - 1, header = header_end;
            while (header && s[header - 1] != '\n') --header;
            if (markdown_cells(s, header, header_end, cells) == table->count) {
                table->separator = a == start;
                return;
            }
        }
        if (!a) break;
        b = a - 1;
        a = b;
        while (a && s[a - 1] != '\n') --a;
        if (markdown_cells(s, a, b, cells) != table->count) break;
    }
    table->count = 0;
}
