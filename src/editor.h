#ifndef EDITOR_H
#define EDITOR_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum editor_mode { NORMAL, INSERT, VISUAL, VISUAL_LINE };
/* What :set view= shows: plain source, styled Markdown, or the read-only reading view. */
enum view_mode { VIEW_SOURCE, VIEW_NORMAL, VIEW_READING, VIEW_COUNT };
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
    KEY_CTRL_I,
    KEY_CTRL_O,
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
    KEY_ZOOM_RESET,
    /* Alt+1 to Alt+9: switch pages; the text is the digit. */
    KEY_PAGE
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
    ED_ZOOM_RESET = 512,
    ED_PAGE_MAP = 1024,
    ED_PAGE_SWITCH = 2048, /* to page_target */
    ED_SAVE_ALL = 4096,    /* the other pages; the editor has saved its own */
    ED_QUIT_ALL = 8192,    /* after ED_SAVE_ALL, if every page saved */
    ED_HELP = 16384
};
enum color_scheme {
    COLOR_JOT,
    COLOR_TOKYONIGHT,
    COLOR_CATPPUCCIN,
    COLOR_KANAGAWA,
    COLOR_EVERFOREST,
    COLOR_COUNT
};
const char *editor_colorscheme_name(enum color_scheme scheme);
/* "source", "normal" or "reading", as :set view= and the config file spell them. */
const char *editor_view_name(enum view_mode view);
struct edit_history;
struct editor_register {
    char *text;
    size_t length;
    bool linewise;
};
struct editor {
    char *text, *filename;
    size_t length, capacity, cursor, top, left;
    enum editor_mode mode;
    enum view_mode view;
    enum color_scheme colorscheme;
    bool modified, command_active, quit, crlf;
    /* Another page (1 to 9) with unsaved changes, or 0. The app sets it so quitting
     * protects every page. */
    unsigned other_modified;
    /* Vim's number and relativenumber options: which line numbers the gutter shows. */
    bool number, relativenumber;
    bool light, search_forward, search_word, search_failed, search_pending, search_hidden;
    char command[1024], message[256], search[256], pending[40];
    size_t command_length, anchor, visual_first, visual_last, search_origin;
    size_t match_start, match_end, desired_column;
    unsigned viewport_rows, viewport_columns;
    unsigned requests, put_count, page_target;
    bool put_after, register_linewise;
    /* The unnamed register mirrors the system clipboard. registers[] holds "0 then "a to "z;
     * register_name is the "x prefix of the command being run, or 0. */
    char *register_text;
    size_t register_length;
    char register_name;
    struct editor_register registers[27];
    uint32_t find_char;
    char find_kind, command_kind, find_text[5];
    uint64_t revision, saved_hash, search_serial;
    size_t (*extra_search)(const struct editor *, size_t, bool, void *);
    void *extra_search_data;
    /* Sets the width of the image at a position: "" resets it, "N" or "N%" sets it, and
     * "+N", "-N", "+N%" or "-N%" change it. Returns false when no image is there. */
    bool (*image_resize)(struct editor *, size_t, const char *, void *);
    void *image_resize_data;
    void (*on_save)(void *);
    void *on_save_data;
    struct edit_history *history;
    /* Dot repeat: the keys of the last change, and of the command being typed. */
    unsigned char *dot, *dot_record;
    size_t dot_length, dot_record_length, dot_record_capacity;
    uint64_t dot_revision;
    unsigned char dot_state;
    bool dot_replaying;
    /* Marks 'a to 'z, the '' context mark and the jump list hold positions plus one, so 0
     * means unset. Edits shift them with the text. */
    size_t marks[26], context, jumps[100];
    unsigned jump_count, jump_index;
    /* R: insert mode that types over text. Backspace restores the characters it replaced,
     * which are kept from replace_start to the end of that line. */
    bool replacing;
    size_t replace_start;
    char *replace_original;
    char *command_history[32];
    unsigned history_count, history_position;
};
bool editor_open(struct editor *ed, const char *filename);
void editor_destroy(struct editor *ed);
void editor_input(struct editor *ed, enum editor_key key, const char *text);
/* Write the text to filename. Saving the open file refuses, with a message, when another
 * program or Jot window changed it since it was read or saved, unless force is set (:w!). */
bool editor_write(struct editor *ed, const char *filename, bool force);
bool editor_save(struct editor *ed, const char *filename);
bool editor_reload(struct editor *ed);
void editor_command(struct editor *ed, const char *command);
bool editor_match_at(const struct editor *ed, size_t position);
char editor_fence(const struct editor *ed, size_t position);
bool editor_heading(const struct editor *ed, size_t position);
void editor_search(struct editor *ed, bool forward, unsigned count);
bool editor_selection(const struct editor *ed, size_t *from, size_t *to, bool *linewise);
/* Drop register contents that mention text, such as a deleted image's asset name. */
void editor_forget_registers(struct editor *ed, const char *text);
/* Report read-only polish mode before starting an edit or import. */
bool editor_colorscheme(struct editor *ed, const char *name);
bool editor_can_edit(struct editor *ed);
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
