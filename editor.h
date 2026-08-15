#ifndef EDITOR_H
#define EDITOR_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum editor_mode { NORMAL, INSERT };
enum editor_key { KEY_TEXT, KEY_ESCAPE, KEY_ENTER, KEY_BACKSPACE, KEY_DELETE,
                  KEY_LEFT, KEY_DOWN, KEY_UP, KEY_RIGHT, KEY_TAB };
struct editor {
    char *text, *filename;
    size_t length, capacity, cursor, top, left;
    enum editor_mode mode;
    bool modified, command_active, quit;
    char command[1024], message[256];
    size_t command_length;
};
bool editor_open(struct editor *ed, const char *filename);
void editor_destroy(struct editor *ed);
void editor_input(struct editor *ed, enum editor_key key, const char *text);
size_t editor_next(const struct editor *ed, size_t pos);
size_t editor_previous(const struct editor *ed, size_t pos);
size_t editor_line_start(const struct editor *ed, size_t pos);
size_t editor_line_end(const struct editor *ed, size_t pos);
size_t editor_column(const struct editor *ed, size_t pos);
uint32_t editor_codepoint(const char *text, size_t length, size_t *offset);
#endif
