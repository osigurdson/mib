#ifndef MARKDOWN_H
#define MARKDOWN_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
enum {
    MD_BOLD = 1,
    MD_ITALIC = 2,
    MD_CODE = 4,
    MD_DIM = 8,
    MD_LINK = 16,
    MD_HEADING = 32,
    MD_DONE = 64,
    MD_QUOTE = 128
};
struct md_span {
    size_t start, end;
    unsigned style;
};
/* Fixed caller-owned span storage; excess styling is safely omitted. */
size_t markdown_line(const char *text, size_t start, size_t end, struct md_span *spans,
                     size_t capacity, bool fenced);
#endif
