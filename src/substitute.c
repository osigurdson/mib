/* Bounded byte-oriented Vim-style regex subset for :s; no regex library. */
#include "edit-internal.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define NODES 256
#define CODE 768
#define GROUPS 10
struct node {
    int type, a, b, value;
    unsigned char bits[32];
};
struct parser {
    const char *p;
    struct node nodes[NODES];
    int used, groups;
    bool bad;
};
enum {
    EMPTY,
    CHAR,
    DOT,
    CLASS,
    BOL,
    EOL,
    WORD_START,
    WORD_END,
    SEQ,
    ALT,
    STAR,
    PLUS,
    MAYBE,
    GROUP
};
struct instruction {
    int op, a, b;
    unsigned char bits[32];
};
enum { I_CHAR, I_DOT, I_CLASS, I_BOL, I_EOL, I_WS, I_WE, I_SPLIT, I_JUMP, I_SAVE, I_MATCH };
struct program {
    struct instruction code[CODE];
    int used;
    bool bad, insensitive;
};
static int node(struct parser *p, int type, int a, int b, int value)
{
    if (p->used == NODES) {
        p->bad = true;
        return 0;
    }
    int n = p->used++;
    p->nodes[n] = (struct node){.type = type, .a = a, .b = b, .value = value};
    return n;
}
static bool token(struct parser *p, char c) { return p->p[0] == '\\' && p->p[1] == c; }
static int expression(struct parser *p);
static int sequence(struct parser *p)
{
    int head = node(p, EMPTY, 0, 0, 0);
    while (*p->p && !token(p, ')') && !token(p, '|') && !p->bad) {
        int n;
        unsigned char c = (unsigned char)*p->p++;
        if (c == '\\' && *p->p == '(') {
            ++p->p;
            int group = ++p->groups;
            if (group >= GROUPS) {
                p->bad = true;
                break;
            }
            int child = expression(p);
            if (!token(p, ')')) {
                p->bad = true;
                break;
            }
            p->p += 2;
            n = node(p, GROUP, child, 0, group);
        } else if (c == '[') {
            n = node(p, CLASS, 0, 0, 0);
            bool invert = *p->p == '^';
            if (invert)
                ++p->p;
            bool first = true;
            while (*p->p && (*p->p != ']' || first)) {
                unsigned char lo = (unsigned char)*p->p++, hi = lo;
                first = false;
                if (lo == '\\' && *p->p)
                    lo = hi = (unsigned char)*p->p++;
                if (*p->p == '-' && p->p[1] && p->p[1] != ']') {
                    ++p->p;
                    hi = (unsigned char)*p->p++;
                }
                for (unsigned v = lo; v <= hi; ++v)
                    p->nodes[n].bits[v / 8] |= (unsigned char)(1u << (v % 8));
            }
            if (*p->p != ']') {
                p->bad = true;
                break;
            }
            ++p->p;
            if (invert)
                for (unsigned i = 0; i < 32; ++i)
                    p->nodes[n].bits[i] ^= 255;
        } else if (c == '\\' && (*p->p == '<' || *p->p == '>'))
            n = node(p, *p->p++ == '<' ? WORD_START : WORD_END, 0, 0, 0);
        else if (c == '\\' && (*p->p == 'd' || *p->p == 'w' || *p->p == 's')) {
            char kind = *p->p++;
            n = node(p, CLASS, 0, 0, 0);
            for (unsigned v = 0; v < 256; ++v)
                if (kind == 'd' ? isdigit(v) : kind == 'w' ? isalnum(v) || v == '_' : isspace(v))
                    p->nodes[n].bits[v / 8] |= (unsigned char)(1u << (v % 8));
        } else {
            if (c == '\\' && *p->p) {
                c = (unsigned char)*p->p++;
                if (c == 't')
                    c = '\t';
                n = node(p, CHAR, 0, 0, c);
            } else
                n = node(p, c == '.' ? DOT : c == '^' ? BOL : c == '$' ? EOL : CHAR, 0, 0, c);
        }
        if (*p->p == '*') {
            ++p->p;
            n = node(p, STAR, n, 0, 0);
        } else if (token(p, '+') || token(p, '?')) {
            char quantifier = p->p[1];
            p->p += 2;
            n = node(p, quantifier == '+' ? PLUS : MAYBE, n, 0, 0);
        }
        head = node(p, SEQ, head, n, 0);
    }
    return head;
}
static int expression(struct parser *p)
{
    int a = sequence(p);
    while (token(p, '|') && !p->bad) {
        p->p += 2;
        a = node(p, ALT, a, sequence(p), 0);
    }
    return a;
}
static int emit(struct program *p, int op, int a, int b)
{
    if (p->used == CODE) {
        p->bad = true;
        return 0;
    }
    int i = p->used++;
    p->code[i] = (struct instruction){.op = op, .a = a, .b = b};
    return i;
}
static void compile(struct program *out, const struct parser *p, int id)
{
    if (out->bad)
        return;
    const struct node *n = &p->nodes[id];
    int split, jump, start;
    switch (n->type) {
    case EMPTY:
        break;
    case SEQ:
        compile(out, p, n->a);
        compile(out, p, n->b);
        break;
    case ALT:
        split = emit(out, I_SPLIT, out->used + 1, 0);
        compile(out, p, n->a);
        jump = emit(out, I_JUMP, 0, 0);
        out->code[split].b = out->used;
        compile(out, p, n->b);
        out->code[jump].a = out->used;
        break;
    case STAR:
    case PLUS:
    case MAYBE:
        start = out->used;
        split = -1;
        if (n->type != PLUS)
            split = emit(out, I_SPLIT, out->used + 1, 0);
        compile(out, p, n->a);
        if (n->type == STAR)
            emit(out, I_JUMP, start, 0);
        if (n->type == PLUS)
            emit(out, I_SPLIT, start, out->used + 1);
        if (split >= 0)
            out->code[split].b = out->used;
        break;
    case GROUP:
        emit(out, I_SAVE, n->value * 2, 0);
        compile(out, p, n->a);
        emit(out, I_SAVE, n->value * 2 + 1, 0);
        break;
    default: {
        int op = n->type == CHAR         ? I_CHAR
                 : n->type == DOT        ? I_DOT
                 : n->type == CLASS      ? I_CLASS
                 : n->type == BOL        ? I_BOL
                 : n->type == EOL        ? I_EOL
                 : n->type == WORD_START ? I_WS
                                         : I_WE;
        int i = emit(out, op, n->value, 0);
        memcpy(out->code[i].bits, n->bits, 32);
        break;
    }
    }
}
struct state {
    size_t at, captures[GROUPS * 2];
    int pc;
};
static bool isword(unsigned char c) { return isalnum(c) || c == '_' || c >= 128; }
static int run(const struct program *prog, const char *text, size_t start, size_t end, size_t at,
               size_t *captures)
{
    /* Backtracking limits also stop nested/empty quantifiers and hostile patterns. */
    struct state stack[128], state = {.at = at, .pc = 0};
    unsigned depth = 0, steps = 0;
    for (unsigned i = 0; i < GROUPS * 2; ++i)
        state.captures[i] = SIZE_MAX;
    state.captures[0] = at;
    for (;;) {
        if (++steps > 1000000)
            return -1;
        const struct instruction *i = &prog->code[state.pc++];
        unsigned char c = state.at < end ? (unsigned char)text[state.at] : 0;
        bool ok = true;
        switch (i->op) {
        case I_CHAR:
            ok = state.at < end && (prog->insensitive ? tolower(c) == tolower(i->a) : c == i->a);
            if (ok)
                ++state.at;
            break;
        case I_DOT:
            ok = state.at < end;
            if (ok)
                ++state.at;
            break;
        case I_CLASS:
            ok = state.at < end && ((i->bits[c / 8] >> (c % 8)) & 1);
            if (!ok && prog->insensitive && state.at < end) {
                unsigned char d = (unsigned char)(isupper(c) ? tolower(c) : toupper(c));
                ok = (i->bits[d / 8] >> (d % 8)) & 1;
            }
            if (ok)
                ++state.at;
            break;
        case I_BOL:
            ok = state.at == start;
            break;
        case I_EOL:
            ok = state.at == end;
            break;
        case I_WS:
            ok = state.at < end && isword(c) &&
                 (state.at == start || !isword((unsigned char)text[state.at - 1]));
            break;
        case I_WE:
            ok = state.at > start && isword((unsigned char)text[state.at - 1]) &&
                 (state.at == end || !isword(c));
            break;
        case I_SAVE:
            state.captures[i->a] = state.at;
            break;
        case I_SPLIT:
            if (depth == 128)
                return -1;
            stack[depth] = state;
            stack[depth++].pc = i->b;
            state.pc = i->a;
            break;
        case I_JUMP:
            state.pc = i->a;
            break;
        case I_MATCH:
            state.captures[1] = state.at;
            memcpy(captures, state.captures, sizeof(state.captures));
            return 1;
        }
        if (!ok) {
            if (!depth)
                return 0;
            state = stack[--depth];
        }
    }
}
static bool field(const char **input, char delimiter, char *out, size_t capacity)
{
    const char *p = *input;
    size_t n = 0;
    while (*p && *p != delimiter) {
        if (*p == '\\' && p[1] == delimiter)
            ++p;
        else if (*p == '\\' && p[1]) {
            if (n + 1 >= capacity)
                return false;
            out[n++] = *p++;
        }
        if (n + 1 >= capacity)
            return false;
        out[n++] = *p++;
    }
    out[n] = 0;
    if (*p == delimiter)
        ++p;
    *input = p;
    return true;
}
void editor_substitute(struct editor *ed, const char *command, size_t first, size_t last)
{
    char pattern[256], replacement[512];
    const char *p = command + 2;
    char delimiter = command[1];
    if (!field(&p, delimiter, pattern, sizeof(pattern)) ||
        !field(&p, delimiter, replacement, sizeof(replacement)))
        goto invalid;
    bool global = false, insensitive = false;
    for (; *p; ++p) {
        if (*p == 'g')
            global = true;
        else if (*p == 'i')
            insensitive = true;
        else if (*p != ' ')
            goto invalid;
    }
    if (!pattern[0])
        snprintf(pattern, sizeof(pattern), "%s", ed->search);
    if (!pattern[0])
        goto invalid;
    struct parser *parser = calloc(1, sizeof(*parser));
    struct program *prog = calloc(1, sizeof(*prog));
    if (!parser || !prog) {
        free(parser);
        free(prog);
        return;
    }
    parser->p = pattern;
    int root = expression(parser);
    compile(prog, parser, root);
    emit(prog, I_MATCH, 0, 0);
    prog->insensitive = insensitive;
    bool bad = parser->bad || *parser->p || prog->bad;
    free(parser);
    if (bad) {
        free(prog);
        goto invalid;
    }
    snprintf(ed->search, sizeof(ed->search), "%s", pattern);
    size_t line = first, changes = 0, original_cursor = ed->cursor;
    bool limited = false;
    editor_change_begin(ed);
    while (line <= last && line < editor_line_count(ed)) {
        size_t start = editor_line_at(ed, line), end = editor_line_end(ed, start), at = start;
        while (at <= end) {
            size_t cap[GROUPS * 2];
            int result = run(prog, ed->text, start, end, at, cap);
            if (result < 0) {
                limited = true;
                break;
            }
            if (!result) {
                if (at == end)
                    break;
                at = editor_next(ed, at);
                continue;
            }
            char *out = NULL;
            size_t length = 0, capacity = 0;
            bool ok = true;
            for (size_t i = 0; replacement[i]; ++i) {
                const char *part = replacement + i;
                size_t n = 1;
                char escaped;
                int group = -1;
                if (replacement[i] == '&')
                    group = 0;
                else if (replacement[i] == '\\' && replacement[i + 1]) {
                    char c = replacement[++i];
                    if (c >= '0' && c <= '9')
                        group = c - '0';
                    else {
                        escaped = c == 't' ? '\t' : c == 'r' ? '\n' : c;
                        part = &escaped;
                    }
                }
                if (group >= 0) {
                    size_t a = cap[group * 2], b = cap[group * 2 + 1];
                    if (a == SIZE_MAX || b == SIZE_MAX)
                        n = 0;
                    else {
                        part = ed->text + a;
                        n = b - a;
                    }
                }
                if (length + n > 2 * 1024 * 1024) {
                    ok = false;
                    break;
                }
                if (length + n > capacity) {
                    size_t next = capacity ? capacity * 2 : 64;
                    while (next < length + n)
                        next *= 2;
                    char *q = realloc(out, next);
                    if (!q) {
                        ok = false;
                        break;
                    }
                    out = q;
                    capacity = next;
                }
                if (n)
                    memcpy(out + length, part, n);
                length += n;
            }
            size_t removed = cap[1] - at;
            if (!ok || !editor_replace(ed, at, cap[1], out ? out : "", length)) {
                free(out);
                limited = true;
                break;
            }
            free(out);
            ++changes;
            end = end - removed + length;
            at += length;
            if (!global)
                break;
            if (!removed) {
                if (at >= end)
                    break;
                at = editor_next(ed, at);
            }
        }
        if (limited)
            break;
        ++line;
    }
    ed->cursor = original_cursor > ed->length ? ed->length : original_cursor;
    editor_clamp(ed);
    editor_change_end(ed);
    free(prog);
    snprintf(ed->message, sizeof(ed->message), "%zu substitution%s%s", changes,
             changes == 1 ? "" : "s", limited ? " (pattern or allocation limit reached)" : "");
    return;
invalid:
    snprintf(ed->message, sizeof(ed->message), "Invalid or unsupported substitution");
}
