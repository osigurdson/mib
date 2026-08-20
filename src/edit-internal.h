#ifndef EDIT_INTERNAL_H
#define EDIT_INTERNAL_H
#include "editor.h"
bool editor_reserve(struct editor *ed, size_t needed);
bool editor_raw_replace(struct editor *ed, size_t from, size_t to, const char *text, size_t length);
uint64_t editor_hash(const struct editor *ed);
bool editor_change_active(const struct editor *ed);
void editor_history_clear(struct editor *ed);
void editor_clamp(struct editor *ed);
/* Keep marks and jumps on the same text after replacing from..to with length bytes. */
void editor_shift_marks(struct editor *ed, size_t from, size_t to, size_t length);
/* Record a jump that started at origin, for '' and Ctrl-O. */
void editor_jump_from(struct editor *ed, size_t origin);
/* Store text in the command's register; yanks without a register also fill "0. */
void editor_yank(struct editor *ed, size_t from, size_t to, bool linewise, bool yank);
void editor_indent(struct editor *ed, size_t first, size_t last, bool right);
void editor_join(struct editor *ed, size_t first, size_t last);
void editor_substitute(struct editor *ed, const char *command, size_t first, size_t last);
#endif
