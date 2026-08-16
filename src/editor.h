#ifndef EDITOR_H
#define EDITOR_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum editor_mode { NORMAL, INSERT, VISUAL, VISUAL_LINE };
enum editor_key {
    KEY_TEXT,
    KEY_ESCAPE,
    KEY_ENTER,
    KEY_BACKSPACE,
    KEY_DELETE,
    KEY_LEFT,
    KEY_DOWN,
    KEY_UP,
    KEY_RIGHT,
    KEY_TAB,
    KEY_SHIFT_TAB,
    KEY_HOME,
    KEY_END,
    KEY_PAGE_UP,
    KEY_PAGE_DOWN,
    KEY_CTRL_A,
    KEY_CTRL_B,
    KEY_CTRL_C,
    KEY_CTRL_D,
    KEY_CTRL_E,
    KEY_CTRL_F,
    KEY_CTRL_Q,
    KEY_CTRL_R,
    KEY_CTRL_S,
    KEY_CTRL_U,
    KEY_CTRL_V,
    KEY_CTRL_W,
    KEY_CTRL_Y,
    KEY_COPY,
    KEY_CUT,
    KEY_COPY_IMAGE_TEXT,
    KEY_ZOOM_IN,
    KEY_ZOOM_OUT,
    KEY_ZOOM_RESET
};
enum editor_request {
    ED_COPY = 1,
    ED_PASTE = 2,
    ED_OPEN = 4,
    ED_OPEN_DIRECTORY = 8,
    ED_RELOAD = 16,
    ED_SAVE = 32,
    ED_COPY_OCR = 64,
    ED_ZOOM_IN = 128,
    ED_ZOOM_OUT = 256,
    ED_ZOOM_RESET = 512
};
struct edit_history;
struct editor {
    char *text, *filename;
    size_t length, capacity, cursor, top, left;
    enum editor_mode mode;
    bool modified, command_active, quit, crlf;
    bool line_numbers, markdown, search_highlight, search_forward, search_word, search_failed,
        search_pending;
    char command[1024], message[256], search[256], pending[40];
    size_t command_length, anchor, visual_first, visual_last, search_origin;
    size_t match_start, match_end, desired_column;
    unsigned viewport_rows, viewport_columns;
    unsigned requests, put_count;
    bool put_after, register_linewise;
    char *register_text;
    size_t register_length;
    uint32_t find_char;
    char find_kind, command_kind, find_text[5];
    uint64_t revision, saved_hash, search_serial;
    size_t (*extra_search)(const struct editor *, size_t, bool, void *);
    void *extra_search_data;
    struct edit_history *history;
    char *command_history[32];
    unsigned history_count, history_position;
};
bool editor_open(struct editor *ed, const char *filename);
void editor_destroy(struct editor *ed);
void editor_input(struct editor *ed, enum editor_key key, const char *text);
bool editor_save(struct editor *ed, const char *filename);
bool editor_reload(struct editor *ed);
void editor_command(struct editor *ed, const char *command);
bool editor_match_at(const struct editor *ed, size_t position);
char editor_fence(const struct editor *ed, size_t position);
bool editor_heading(const struct editor *ed, size_t position);
void editor_search(struct editor *ed, bool forward, unsigned count);
bool editor_selection(const struct editor *ed, size_t *from, size_t *to, bool *linewise);
bool editor_replace(struct editor *ed, size_t from, size_t to, const char *text, size_t length);
void editor_put(struct editor *ed, const char *text, size_t length, bool linewise, bool after,
                unsigned count);
void editor_undo(struct editor *ed, bool redo);
void editor_change_begin(struct editor *ed);
void editor_change_end(struct editor *ed);
size_t editor_next(const struct editor *ed, size_t pos);
size_t editor_previous(const struct editor *ed, size_t pos);
size_t editor_line_start(const struct editor *ed, size_t pos);
size_t editor_line_end(const struct editor *ed, size_t pos);
size_t editor_line_at(const struct editor *ed, size_t line);
size_t editor_line_number(const struct editor *ed, size_t pos);
size_t editor_line_count(const struct editor *ed);
size_t editor_column(const struct editor *ed, size_t pos);
uint32_t editor_codepoint(const char *text, size_t length, size_t *offset);
#endif
