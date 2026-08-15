#define _GNU_SOURCE
#include "editor.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void type(struct editor *ed, const char *text) { editor_input(ed, KEY_TEXT, text); }
static void escape(struct editor *ed) { editor_input(ed, KEY_ESCAPE, ""); }
static void command(struct editor *ed, const char *text)
{
    type(ed, ":"); type(ed, text); editor_input(ed, KEY_ENTER, "");
}

int main(void)
{
    char directory[] = "/tmp/tiny-vi-test-XXXXXX";
    assert(mkdtemp(directory));
    char path[512], alias[512];
    snprintf(path, sizeof(path), "%s/file.txt", directory);
    snprintf(alias, sizeof(alias), "%s/link.txt", directory);
    struct editor ed = {0};
    assert(editor_open(&ed, NULL));
    assert(ed.mode == NORMAL && ed.length == 0);
    type(&ed, "i");
    assert(ed.mode == INSERT && ed.length == 0);
    type(&ed, "hello"); editor_input(&ed, KEY_ENTER, ""); type(&ed, "world");
    assert(!strcmp(ed.text, "hello\nworld"));
    escape(&ed);
    assert(ed.cursor == 10);
    type(&ed, "k"); assert(ed.cursor == 4);
    type(&ed, "h"); assert(ed.cursor == 3);
    type(&ed, "j"); assert(ed.cursor == 9);
    type(&ed, "l"); type(&ed, "l"); assert(ed.cursor == 10);
    command(&ed, "q"); assert(!ed.quit && strstr(ed.message, "Unsaved"));
    command(&ed, "w"); assert(ed.modified && strstr(ed.message, "No filename"));
    char save_command[600];
    snprintf(save_command, sizeof(save_command), "w %s", path);
    command(&ed, save_command);
    assert(!ed.modified && !strcmp(ed.filename, path));
    struct stat st; assert(stat(path, &st) == 0 && st.st_size == 11);
    assert(chmod(path, 0640) == 0);
    editor_destroy(&ed);

    ed = (struct editor){0};
    assert(editor_open(&ed, path));
    assert(!strcmp(ed.text, "hello\nworld"));
    ed.cursor = 6; type(&ed, "i"); editor_input(&ed, KEY_BACKSPACE, "");
    assert(!strcmp(ed.text, "helloworld"));
    editor_input(&ed, KEY_DELETE, ""); assert(!strcmp(ed.text, "helloorld"));
    escape(&ed);
    command(&ed, "wq /does-not-exist/tiny-vi.txt");
    assert(!ed.quit && ed.modified && strstr(ed.message, "Write:"));
    command(&ed, "wq"); assert(ed.quit && !ed.modified);
    assert(stat(path, &st) == 0 && (st.st_mode & 0777) == 0640);
    editor_destroy(&ed);

    assert(symlink(path, alias) == 0);
    ed = (struct editor){0}; assert(editor_open(&ed, alias));
    type(&ed, "i"); type(&ed, "é");
    editor_input(&ed, KEY_BACKSPACE, ""); assert(!strcmp(ed.text, "helloorld"));
    type(&ed, "é"); escape(&ed); command(&ed, "w");
    assert(!ed.modified && lstat(alias, &st) == 0 && S_ISLNK(st.st_mode));
    command(&ed, "q"); assert(ed.quit);
    editor_destroy(&ed);

    ed = (struct editor){0}; assert(editor_open(&ed, NULL));
    type(&ed, "i"); type(&ed, "\tα\nxy"); escape(&ed);
    type(&ed, "k"); assert(editor_column(&ed, ed.cursor) == 0);
    type(&ed, "l"); assert(editor_column(&ed, ed.cursor) == 4);
    type(&ed, "j"); assert(ed.cursor == ed.length - 1);
    type(&ed, ":"); type(&ed, "é"); editor_input(&ed, KEY_BACKSPACE, "");
    assert(ed.command_length == 0 && ed.command[0] == '\0');
    escape(&ed); command(&ed, "q!"); assert(ed.quit);
    editor_destroy(&ed);

    ed = (struct editor){0}; assert(!editor_open(&ed, directory)); editor_destroy(&ed);
    FILE *binary = fopen(path, "wb"); assert(binary);
    assert(fwrite("a\0b", 1, 3, binary) == 3); fclose(binary);
    ed = (struct editor){0}; assert(!editor_open(&ed, path)); editor_destroy(&ed);
    unlink(alias); unlink(path); rmdir(directory);
    puts("Editor tests passed");
}
