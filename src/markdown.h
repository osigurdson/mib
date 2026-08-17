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
    MD_QUOTE = 128,
    MD_HIDE = 256,
    MD_BULLET = 512,
    MD_TASK = 1024,
    MD_CHECKED = 2048,
    MD_STRIKE = 4096
};
struct md_span {
    size_t start, end;
    unsigned style;
};
/* One logical line of presentation data; no document-sized render buffer. */
struct md_cache {
    struct md_span spans[128];
    size_t start, end, count;
    uint64_t revision;
    char fence_after;
    bool valid, fenced, fence_line, quote;
    unsigned indent, heading;
    bool rule;
};
struct md_block { unsigned indent, heading; bool rule; };
struct md_block markdown_block(const char *text, size_t start, size_t end);
/* Fixed caller-owned span storage; excess styling is safely omitted. */
size_t markdown_line(const char *text, size_t start, size_t end, struct md_span *spans,
                     size_t capacity, bool fenced);
#endif
