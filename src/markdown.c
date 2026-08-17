#include "markdown.h"
#include <ctype.h>
#include <string.h>
static bool boundary(unsigned char c) { return !isalnum(c) && c != '_' && c < 128; }
static void add(struct md_span *out, size_t *n, size_t capacity, size_t a, size_t b, unsigned flags)
{
    if (a < b && *n < capacity)
        out[(*n)++] = (struct md_span){a, b, flags};
}
size_t markdown_line(const char *s, size_t start, size_t end, struct md_span *out, size_t capacity,
                     bool fenced)
{
    size_t n = 0, p = start;
    while (p < end && s[p] == ' ')
        ++p;
    if (fenced) {
        add(out, &n, capacity, start, end, MD_CODE);
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
    if (p + 1 < end && strchr("-*+", s[p]) && s[p + 1] == ' ') {
        bool task = p + 5 < end && s[p + 2] == '[' && s[p + 4] == ']' && s[p + 5] == ' ';
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
    for (size_t i = start; i < end; ++i) {
        if (s[i] == '\\') {
            if (i + 1 < end)
                add(out, &n, capacity, i, i + 1, MD_HIDE);
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
                add(out, &n, capacity, i, i + run, MD_DIM | MD_HIDE);
                add(out, &n, capacity, i + run, q, MD_CODE);
                add(out, &n, capacity, q, q + run, MD_DIM | MD_HIDE);
                i = q + run - 1;
            }
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
                    add(out, &n, capacity, i, a + 1, MD_DIM | conceal);
                    add(out, &n, capacity, a + 1, q, MD_LINK);
                    add(out, &n, capacity, q, r + 1, MD_DIM | conceal);
                    i = r;
                }
            }
            continue;
        }
        if (s[i] == '*' || s[i] == '_') {
            char c = s[i];
            size_t run = i + 1 < end && s[i + 1] == c ? 2 : 1;
            if ((i > start && !boundary((unsigned char)s[i - 1])) || i + run >= end ||
                isspace((unsigned char)s[i + run]))
                continue;
            size_t q = i + run;
            while (q + run <= end) {
                if (s[q] == c && (run == 1 || s[q + 1] == c) && q > i + run &&
                    !isspace((unsigned char)s[q - 1]) &&
                    (q + run == end || boundary((unsigned char)s[q + run])))
                    break;
                ++q;
            }
            if (q + run <= end) {
                add(out, &n, capacity, i, i + run, MD_DIM | MD_HIDE);
                add(out, &n, capacity, i + run, q, run == 2 ? MD_BOLD : MD_ITALIC);
                add(out, &n, capacity, q, q + run, MD_DIM | MD_HIDE);
                i = q + run - 1;
            }
        }
    }
    return n;
}
