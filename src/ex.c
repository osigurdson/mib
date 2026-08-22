#include "edit-internal.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *editor_colorscheme_name(enum color_scheme scheme)
{
    static const char *const names[] = {"jot", "tokyonight", "catppuccin", "kanagawa",
                                        "everforest"};
    return (unsigned)scheme < COLOR_COUNT ? names[scheme] : "jot";
}
const char *editor_view_name(enum view_mode view)
{
    static const char *const names[] = {"source", "normal", "reading"};
    return (unsigned)view < VIEW_COUNT ? names[view] : "normal";
}
bool editor_colorscheme(struct editor *ed, const char *name)
{
    for (unsigned i = 0; i < COLOR_COUNT; ++i) {
        if (strcmp(name, editor_colorscheme_name((enum color_scheme)i)))
            continue;
        ed->colorscheme = (enum color_scheme)i;
        return true;
    }
    return false;
}
static size_t address(struct editor *ed, const char **input, bool *present)
{
    const char *p = *input;
    size_t line = editor_line_number(ed, ed->cursor);
    *present = true;
    if (*p == '.')
        ++p;
    else if (*p == '$') {
        line = editor_line_count(ed) - 1;
        ++p;
    } else if (p[0] == '\'' && (p[1] == '<' || p[1] == '>')) {
        line = editor_line_number(ed, p[1] == '<'       ? ed->visual_first
                                      : ed->visual_last ? editor_previous(ed, ed->visual_last)
                                                        : 0);
        p += 2;
    } else if (isdigit((unsigned char)*p)) {
        unsigned long n = strtoul(p, (char **)&p, 10);
        line = n ? n - 1 : 0;
    } else if (*p != '+' && *p != '-')
        *present = false;
    while (*p == '+' || *p == '-') {
        bool add = *p++ == '+';
        unsigned long n = 1;
        if (isdigit((unsigned char)*p))
            n = strtoul(p, (char **)&p, 10);
        if (n > 100000000)
            n = 100000000;
        line = add ? line + n : n > line ? 0 : line - n;
    }
    if (line >= editor_line_count(ed))
        line = editor_line_count(ed) - 1;
    *input = p;
    return line;
}
/* Quitting closes every page, so unsaved changes in another page block it too. */
static void quit(struct editor *ed)
{
    unsigned page = ed->other_modified;
    if (ed->modified)
        snprintf(ed->message, sizeof(ed->message), "Unsaved changes (:w to save, :q! to discard)");
    else if (page)
        snprintf(ed->message, sizeof(ed->message),
                 "Unsaved changes in page %u (Alt+%u to visit, :q! to discard)", page, page);
    else
        ed->quit = true;
}
void editor_command(struct editor *ed, const char *command)
{
    char buffer[1024];
    snprintf(buffer, sizeof(buffer), "%s", command);
    const char *p = buffer;
    while (*p == ' ' || *p == ':')
        ++p;
    size_t first = editor_line_number(ed, ed->cursor), last = first;
    bool range = false;
    if (*p == '%') {
        first = 0;
        last = editor_line_count(ed) - 1;
        range = true;
        ++p;
    } else {
        first = address(ed, &p, &range);
        last = first;
        if (*p == ',') {
            bool present;
            ++p;
            last = address(ed, &p, &present);
            if (!present)
                goto unknown;
            range = true;
        }
    }
    if (first > last) {
        size_t tmp = first;
        first = last;
        last = tmp;
    }
    while (*p == ' ')
        ++p;
    if (!*p && range) {
        editor_jump_from(ed, ed->cursor);
        ed->cursor = editor_line_at(ed, last);
        editor_clamp(ed);
        return;
    }
    if (*p == 's' && p[1] && !isalnum((unsigned char)p[1]) && !isspace((unsigned char)p[1])) {
        if (!editor_can_edit(ed))
            return;
        editor_substitute(ed, p, first, last);
        return;
    }
    char *argument = strchr(p, ' ');
    if (argument) {
        *argument++ = 0;
        while (*argument == ' ')
            ++argument;
    }
    if (!strcmp(p, "w") || !strcmp(p, "wq") || !strcmp(p, "x") || !strcmp(p, "w!") ||
        !strcmp(p, "wq!") || !strcmp(p, "x!")) {
        if (argument && *argument) {
            snprintf(ed->message, sizeof(ed->message), "Save uses the current page or file");
            return;
        }
        /* The ! overwrites a file changed on disk since it was read. */
        bool leave = p[0] != 'w' || p[1] == 'q', force = p[strlen(p) - 1] == '!';
        if ((p[0] == 'x' && !ed->modified) || editor_write(ed, ed->filename, force)) {
            if (leave)
                quit(ed);
        }
    } else if (!strcmp(p, "wa") || !strcmp(p, "wall") || !strcmp(p, "wqa") || !strcmp(p, "wqall") ||
               !strcmp(p, "xa") || !strcmp(p, "xall")) {
        /* Save every page with changes; :wqa and :xa then quit. */
        if (argument && *argument) {
            snprintf(ed->message, sizeof(ed->message), "Save uses the current page or file");
            return;
        }
        if (ed->modified && !editor_save(ed, ed->filename))
            return;
        ed->requests |= ED_SAVE_ALL | (p[0] == 'x' || p[1] == 'q' ? ED_QUIT_ALL : 0);
    } else if (!strcmp(p, "q!") || !strcmp(p, "qa!") || !strcmp(p, "qall!"))
        ed->quit = true;
    else if (!strcmp(p, "q") || !strcmp(p, "qa") || !strcmp(p, "qall"))
        quit(ed); /* :q already closes every page */
    else if (!strcmp(p, "ls") || !strcmp(p, "buffers") || !strcmp(p, "files")) {
        if (argument && *argument)
            goto unknown;
        ed->requests |= ED_PAGE_MAP; /* the list of pages, which stand in for buffers */
    } else if (!strcmp(p, "b") || !strcmp(p, "buffer") ||
               (p[0] == 'b' && p[1] >= '0' && p[1] <= '9' && !p[2])) {
        /* :b N, :bN and :buffer N show page N. */
        const char *number = p[1] && p[1] <= '9' ? p + 1 : argument;
        if (number && number[0] >= '1' && number[0] <= '9' && !number[1] &&
            (number != p + 1 || !argument || !*argument)) {
            ed->page_target = (unsigned)(number[0] - '1');
            ed->requests |= ED_PAGE_SWITCH;
        } else
            snprintf(ed->message, sizeof(ed->message), "Pages are :b 1 to :b 9");
    } else if (*p && strlen(p) <= strlen("help") && !strncmp(p, "help", strlen(p)))
        ed->requests |= ED_HELP; /* :h, :help and :help topic all show the one card */
    else if (!strcmp(p, "e!"))
        editor_reload(ed);
    else if (!strcmp(p, "u") || !strcmp(p, "undo"))
        editor_undo(ed, false);
    else if (!strcmp(p, "redo"))
        editor_undo(ed, true);
    else if (!strcmp(p, "Ex"))
        ed->requests |= ED_OPEN_DIRECTORY;
    else if (strlen(p) >= 4 && strlen(p) <= strlen("colorscheme") &&
             !strncmp(p, "colorscheme", strlen(p))) {
        if (!argument || !*argument)
            snprintf(ed->message, sizeof(ed->message), "%s",
                     editor_colorscheme_name(ed->colorscheme));
        else if (!editor_colorscheme(ed, argument))
            snprintf(ed->message, sizeof(ed->message), "Cannot find color scheme: %.200s",
                     argument);
    } else if (!strcmp(p, "set") && argument) {
        if (!strncmp(argument, "view=", 5)) {
            unsigned view = 0;
            while (view < VIEW_COUNT &&
                   strcmp(argument + 5, editor_view_name((enum view_mode)view)))
                ++view;
            if (view == VIEW_COUNT) {
                snprintf(ed->message, sizeof(ed->message), "Views are source, normal and reading");
                return;
            }
            editor_change_end(ed);
            ed->view = (enum view_mode)view;
            ed->mode = NORMAL;
            ed->pending[0] = 0;
            ed->top = editor_line_start(ed, ed->top);
            editor_clamp(ed);
        } else if (!strcmp(argument, "view?") || !strcmp(argument, "view"))
            snprintf(ed->message, sizeof(ed->message), "view=%s", editor_view_name(ed->view));
        else if (!strcmp(argument, "background=light") || !strcmp(argument, "bg=light"))
            ed->light = true;
        else if (!strcmp(argument, "background=dark") || !strcmp(argument, "bg=dark"))
            ed->light = false;
        else if (!strcmp(argument, "background?") || !strcmp(argument, "bg?") ||
                 !strcmp(argument, "background") || !strcmp(argument, "bg"))
            snprintf(ed->message, sizeof(ed->message), "background=%s",
                     ed->light ? "light" : "dark");
        else
            goto unknown;
    } else if (strlen(p) >= 3 && strlen(p) <= strlen("resize") &&
               !strncmp(p, "resize", strlen(p))) {
        if (!ed->image_resize ||
            !ed->image_resize(ed, ed->cursor, argument ? argument : "", ed->image_resize_data))
            snprintf(ed->message, sizeof(ed->message), "No image under the cursor");
    } else if (strlen(p) == 1 && strchr("dy><j", *p)) {
        if (*p != 'y' && !editor_can_edit(ed))
            return;
        size_t a = editor_line_at(ed, first), b = editor_line_end(ed, editor_line_at(ed, last));
        if (*p == '>' || *p == '<')
            editor_indent(ed, a, b, *p == '>');
        else if (*p == 'j') {
            if (!range)
                b = editor_line_end(ed, b < ed->length ? b + 1 : b);
            editor_join(ed, a, b);
        } else {
            if (b < ed->length)
                ++b;
            editor_yank(ed, a, b, true, *p == 'y');
            if (*p == 'd') {
                if (b == ed->length && a)
                    --a;
                editor_replace(ed, a, b, "", 0);
            }
            ed->cursor = a;
            editor_clamp(ed);
        }
    } else
        goto unknown;
    return;
unknown:
    snprintf(ed->message, sizeof(ed->message), "Unknown command: %.230s", command);
}
