#include "editor.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static struct editor ed;
static void keys(const char *s)
{
    while (*s) {
        char c[2] = {*s++, 0};
        editor_input(&ed, KEY_TEXT, c);
    }
}
static void reset(const char *s)
{
    editor_destroy(&ed);
    ed = (struct editor){0};
    assert(editor_open(&ed, NULL));
    editor_input(&ed, KEY_TEXT, "i");
    editor_input(&ed, KEY_TEXT, s);
    editor_input(&ed, KEY_ESCAPE, "");
    ed.cursor = 0;
}
static void expect(const char *s)
{
    if (strcmp(ed.text, s)) {
        fprintf(stderr, "Expected [%s], got [%s]\n", s, ed.text);
        assert(0);
    }
}
int main(void)
{
    reset("notes");
    const char *removed[] = {"set nu", "set nonu", "set number", "set nonumber", "noh", "nohlsearch"};
    for (unsigned i = 0; i < sizeof(removed) / sizeof(removed[0]); ++i) {
        editor_command(&ed, removed[i]);
        assert(strstr(ed.message, "Unknown command"));
        expect("notes");
    }
    editor_command(&ed, "set nomd");
    assert(!ed.markdown);
    editor_command(&ed, "set md");
    assert(ed.markdown);
    reset("one two three\nfour five\nsix\n");
    keys("2dw");
    expect("three\nfour five\nsix\n");
    keys("u");
    expect("one two three\nfour five\nsix\n");
    keys("d0");
    expect("one two three\nfour five\nsix\n");
    keys("2dd");
    expect("six\n");
    keys("u");
    keys("d3j");
    expect("");
    keys("u");
    keys("ciw");
    keys("hello");
    editor_input(&ed, KEY_ESCAPE, "");
    expect("hello two three\nfour five\nsix\n");
    keys("u");
    expect("one two three\nfour five\nsix\n");
    editor_input(&ed, KEY_CTRL_R, "");
    expect("hello two three\nfour five\nsix\n");
    reset("a (two [three]) tail");
    keys("f(ci(");
    keys("four");
    editor_input(&ed, KEY_ESCAPE, "");
    expect("a (four) tail");
    keys("u");
    expect("a (two [three]) tail");
    keys("%");
    assert(ed.text[ed.cursor] == ')');
    reset("one two\nthree four\nfive");
    keys("vwd");
    expect("wo\nthree four\nfive");
    keys("u");
    keys("Vjy");
    assert(ed.register_linewise && !strcmp(ed.register_text, "one two\nthree four\n"));
    reset("  alpha\n  beta\ngamma");
    keys(">>");
    expect("      alpha\n  beta\ngamma");
    keys("<<");
    expect("  alpha\n  beta\ngamma");
    keys("J");
    expect("  alpha beta\ngamma");
    keys("u");
    keys("A");
    editor_input(&ed, KEY_ENTER, "");
    keys("new");
    editor_input(&ed, KEY_ESCAPE, "");
    expect("  alpha\n  new\n  beta\ngamma");
    keys("u");
    expect("  alpha\n  beta\ngamma");
    reset("foo Foo FOO food\nfoo bar foo");
    keys("/foo");
    assert(ed.cursor == 4);
    editor_input(&ed, KEY_ENTER, "");
    assert(ed.cursor == 4);
    keys("n");
    assert(ed.cursor == 8);
    keys("N");
    assert(ed.cursor == 4);
    keys("/FOO");
    editor_input(&ed, KEY_ENTER, "");
    assert(ed.cursor == 8);
    editor_command(&ed, "%s/\\(foo\\)/[\\1]/gi");
    expect("[foo] [Foo] [FOO] [foo]d\n[foo] bar [foo]");
    keys("u");
    expect("foo Foo FOO food\nfoo bar foo");
    editor_command(&ed, "%s/\\<foo\\>/X/g");
    expect("X Foo FOO food\nX bar X");
    keys("u");
    editor_command(&ed, "%s/f\\(oo\\|ood\\)/<\\1>/g");
    expect("<oo> Foo FOO <oo>d\n<oo> bar <oo>");
    keys("u");
    editor_command(&ed, "2s/o\\+/O/g");
    expect("foo Foo FOO food\nfO bar fO");
    keys("u");
    editor_command(&ed, "%s/^/start /g");
    expect("start foo Foo FOO food\nstart foo bar foo");
    keys("u");
    editor_command(&ed, "1,2>");
    expect("    foo Foo FOO food\n    foo bar foo");
    editor_command(&ed, "%<");
    expect("foo Foo FOO food\nfoo bar foo");
    editor_command(&ed, "2");
    assert(ed.cursor == 17);
    editor_command(&ed, "1d");
    expect("foo bar foo");
    keys("u");
    expect("foo Foo FOO food\nfoo bar foo");
    reset("abc");
    keys("A");
    for (int i = 0; i < 20000; ++i)
        keys("x");
    editor_input(&ed, KEY_ESCAPE, "");
    keys("u");
    expect("abc");
    editor_input(&ed, KEY_CTRL_R, "");
    assert(ed.length == 20003);
    keys("u");
    keys("ix");
    editor_input(&ed, KEY_ESCAPE, "");
    editor_input(&ed, KEY_CTRL_R, "");
    expect("abxc");
    reset("intro\n# One\ntext\n```\n# fake\n```\n## Two\nend");
    keys("]]");
    assert(editor_line_number(&ed, ed.cursor) == 1);
    keys("]]");
    assert(editor_line_number(&ed, ed.cursor) == 6);
    keys("[[");
    assert(editor_line_number(&ed, ed.cursor) == 1);
    reset("  text");
    keys("A");
    editor_input(&ed, KEY_TAB, "");
    expect("  text    ");
    editor_input(&ed, KEY_SHIFT_TAB, "");
    keys("!");
    editor_input(&ed, KEY_ESCAPE, "");
    expect("text    !");
    keys("u");
    expect("  text");
    reset("foo food foo");
    keys("*");
    assert(ed.cursor == 9);
    keys("n");
    assert(ed.cursor == 0);
    reset("a é b é c");
    keys("f");
    editor_input(&ed, KEY_TEXT, "é");
    assert(ed.cursor == 2);
    keys(";");
    assert(ed.cursor == 7);
    keys(",");
    assert(ed.cursor == 2);
    keys(";");
    assert(ed.cursor == 7);
    editor_destroy(&ed);
    puts("Modal operators, search, substitutions, grouped undo and redo passed");
}
