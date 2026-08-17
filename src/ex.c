#include "edit-internal.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
        ed->cursor = editor_line_at(ed, last);
        editor_clamp(ed);
        return;
    }
    if (*p == 's' && p[1] && !isalnum((unsigned char)p[1]) && !isspace((unsigned char)p[1])) {
        editor_substitute(ed, p, first, last);
        return;
    }
    char *argument = strchr(p, ' ');
    if (argument) {
        *argument++ = 0;
        while (*argument == ' ')
            ++argument;
    }
    if (!strcmp(p, "w") || !strcmp(p, "wq") || !strcmp(p, "x")) {
        if (argument && *argument) {
            snprintf(ed->message, sizeof(ed->message), "Save uses the current page or file");
            return;
        }
        bool quit = strcmp(p, "w") != 0;
        if ((!strcmp(p, "x") && !ed->modified) || editor_save(ed, ed->filename)) {
            if (quit)
                ed->quit = true;
        }
    } else if (!strcmp(p, "q!"))
        ed->quit = true;
    else if (!strcmp(p, "q")) {
        if (ed->modified)
            snprintf(ed->message, sizeof(ed->message),
                     "Unsaved changes (:w to save, :q! to discard)");
        else
            ed->quit = true;
    } else if (!strcmp(p, "e!"))
        editor_reload(ed);
    else if (!strcmp(p, "u") || !strcmp(p, "undo"))
        editor_undo(ed, false);
    else if (!strcmp(p, "redo"))
        editor_undo(ed, true);
    else if (!strcmp(p, "Ex"))
        ed->requests |= ED_OPEN_DIRECTORY;
    else if (!strcmp(p, "set") && argument) {
        if (!strcmp(argument, "md"))
            ed->markdown = true;
        else if (!strcmp(argument, "nomd"))
            ed->markdown = false;
        else if (!strcmp(argument, "light"))
            ed->light = true;
        else if (!strcmp(argument, "dark"))
            ed->light = false;
        else
            goto unknown;
    } else if (strlen(p) == 1 && strchr("dy><j", *p)) {
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
            editor_yank(ed, a, b, true);
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
