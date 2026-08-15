#ifndef EDIT_INTERNAL_H
#define EDIT_INTERNAL_H
#include "editor.h"
bool editor_reserve(struct editor *ed, size_t needed);
bool editor_raw_replace(struct editor *ed, size_t from, size_t to, const char *text, size_t length);
uint64_t editor_hash(const struct editor *ed);
void editor_history_clear(struct editor *ed);
void editor_clamp(struct editor *ed);
void editor_yank(struct editor *ed, size_t from, size_t to, bool linewise);
void editor_indent(struct editor *ed, size_t first, size_t last, bool right);
void editor_join(struct editor *ed, size_t first, size_t last);
void editor_substitute(struct editor *ed, const char *command, size_t first, size_t last);
#endif
