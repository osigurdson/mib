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
    type(ed, ":");
    type(ed, text);
    editor_input(ed, KEY_ENTER, "");
}

int main(void)
{
    char directory[] = "/tmp/mib-test-XXXXXX";
    assert(mkdtemp(directory));
    char path[512], alias[512];
    snprintf(path, sizeof(path), "%s/file.txt", directory);
    snprintf(alias, sizeof(alias), "%s/link.txt", directory);
    struct editor ed = {0};
    assert(editor_open(&ed, path));
    assert(ed.mode == NORMAL && ed.length == 0);
    type(&ed, "i");
    assert(ed.mode == INSERT && ed.length == 0);
    type(&ed, "hello");
    editor_input(&ed, KEY_ENTER, "");
    type(&ed, "world");
    assert(!strcmp(ed.text, "hello\nworld"));
    escape(&ed);
    assert(ed.cursor == 10);
    type(&ed, "k");
    assert(ed.cursor == 4);
    type(&ed, "h");
    assert(ed.cursor == 3);
    type(&ed, "j");
    assert(ed.cursor == 9);
    {
        /* A large paste grows the buffer; deleting it lets editor_compact give the space back. */
        size_t before = ed.capacity, big = 1 << 18;
        char *fill = malloc(big);
        assert(fill);
        memset(fill, 'x', big);
        assert(editor_replace(&ed, ed.length, ed.length, fill, big));
        free(fill);
        assert(ed.capacity > big);
        editor_compact(&ed);
        assert(ed.capacity > big);
        assert(editor_replace(&ed, ed.length - big, ed.length, "", 0));
        editor_compact(&ed);
        assert(ed.capacity <= before && ed.capacity > ed.length);
        assert(!strcmp(ed.text, "hello\nworld"));
        editor_undo(&ed, false);
        assert(ed.length == 11 + big && ed.capacity > ed.length);
        editor_undo(&ed, false);
        assert(!strcmp(ed.text, "hello\nworld"));
        ed.cursor = 9;
    }
    type(&ed, "l");
    type(&ed, "l");
    assert(ed.cursor == 10);
    command(&ed, "q");
    assert(!ed.quit && strstr(ed.message, "Unsaved"));
    char save_command[600];
    const char *save_commands[] = {"w", "wq", "x"};
    for (unsigned i = 0; i < 3; ++i) {
        snprintf(save_command, sizeof(save_command), "%s %s", save_commands[i], alias);
        command(&ed, save_command);
        assert(ed.modified && !ed.quit && !strcmp(ed.filename, path));
        assert(strstr(ed.message, "current page or file") && access(alias, F_OK) == -1);
    }
    command(&ed, "w");
    assert(!ed.modified && !strcmp(ed.filename, path));
    struct stat st;
    assert(stat(path, &st) == 0 && st.st_size == 11);
    assert(chmod(path, 0640) == 0);
    editor_destroy(&ed);

    ed = (struct editor){0};
    assert(editor_open(&ed, path));
    assert(!strcmp(ed.text, "hello\nworld"));
    ed.cursor = 6;
    type(&ed, "i");
    editor_input(&ed, KEY_BACKSPACE, "");
    assert(!strcmp(ed.text, "helloworld"));
    editor_input(&ed, KEY_DELETE, "");
    assert(!strcmp(ed.text, "helloorld"));
    escape(&ed);
    command(&ed, "wq /does-not-exist/mib.txt");
    assert(!ed.quit && ed.modified && strstr(ed.message, "current page or file"));
    command(&ed, "wq");
    assert(ed.quit && !ed.modified);
    assert(stat(path, &st) == 0 && (st.st_mode & 0777) == 0640);
    editor_destroy(&ed);

    assert(symlink(path, alias) == 0);
    ed = (struct editor){0};
    assert(editor_open(&ed, alias));
    type(&ed, "i");
    type(&ed, "é");
    editor_input(&ed, KEY_BACKSPACE, "");
    assert(!strcmp(ed.text, "helloorld"));
    type(&ed, "é");
    escape(&ed);
    command(&ed, "w");
    assert(!ed.modified && lstat(alias, &st) == 0 && S_ISLNK(st.st_mode));
    command(&ed, "q");
    assert(ed.quit);
    editor_destroy(&ed);

    ed = (struct editor){0};
    assert(editor_open(&ed, NULL));
    type(&ed, "i");
    type(&ed, "\tα\nxy");
    escape(&ed);
    type(&ed, "k");
    assert(editor_column(&ed, ed.cursor) == 0);
    type(&ed, "l");
    assert(editor_column(&ed, ed.cursor) == 4);
    type(&ed, "j");
    assert(ed.cursor == ed.length - 1);
    type(&ed, ":");
    type(&ed, "é");
    editor_input(&ed, KEY_BACKSPACE, "");
    assert(ed.command_length == 0 && ed.command[0] == '\0');
    escape(&ed);
    command(&ed, "w");
    assert(ed.modified && strstr(ed.message, "No filename"));
    command(&ed, "q!");
    assert(ed.quit);
    editor_destroy(&ed);

    ed = (struct editor){0};
    assert(!editor_open(&ed, directory));
    editor_destroy(&ed);
    FILE *dos = fopen(path, "wb");
    assert(dos);
    assert(fwrite("a\r\nb\r\n", 1, 6, dos) == 6);
    fclose(dos);
    ed = (struct editor){0};
    assert(editor_open(&ed, path));
    assert(ed.crlf && !strcmp(ed.text, "a\nb\n"));
    type(&ed, "i");
    type(&ed, "X");
    escape(&ed);
    command(&ed, "wq");
    assert(ed.quit && !ed.modified);
    editor_destroy(&ed);
    dos = fopen(path, "rb");
    assert(dos);
    char raw[16] = {0};
    assert(fread(raw, 1, sizeof(raw), dos) == 7);
    fclose(dos);
    assert(!memcmp(raw, "Xa\r\nb\r\n", 7));
    /* Two editors on one file, as two mib windows: the second save would lose the first. */
    struct editor a = {0}, b = {0};
    assert(editor_open(&a, path) && editor_open(&b, path));
    type(&a, "i");
    type(&a, "A");
    escape(&a);
    command(&a, "w");
    assert(!a.modified);
    type(&b, "i");
    type(&b, "B");
    escape(&b);
    command(&b, "w");
    assert(b.modified && strstr(b.message, "Changed on disk"));
    command(&b, "wq");
    assert(!b.quit && b.modified);
    command(&b, "w!");
    assert(!b.modified); /* overwrites, as asked */
    type(&a, "i");
    type(&a, "C");
    escape(&a);
    command(&a, "x");
    assert(!a.quit && strstr(a.message, "Changed on disk"));
    command(&a, "e!");
    assert(!a.modified && a.text[0] == 'B');
    type(&a, "i");
    type(&a, "C");
    escape(&a);
    command(&a, "x");
    assert(a.quit);
    /* Saving the text the file already holds loses nothing, so it is not a conflict. */
    type(&b, "i");
    type(&b, "C");
    escape(&b);
    command(&b, "wq");
    assert(b.quit);
    editor_destroy(&a);
    editor_destroy(&b);
    FILE *binary = fopen(path, "wb");
    assert(binary);
    assert(fwrite("a\0b", 1, 3, binary) == 3);
    fclose(binary);
    ed = (struct editor){0};
    assert(!editor_open(&ed, path));
    editor_destroy(&ed);
    /* Files up to the limit open; a byte more is refused, with a message, before it's read. */
    for (unsigned extra = 0; extra < 2; ++extra) {
        FILE *big = fopen(path, "wb");
        static char line[1 << 16];
        memset(line, 'a', sizeof(line) - 1);
        line[sizeof(line) - 1] = '\n';
        for (unsigned i = 0; i < EDITOR_FILE_LIMIT / sizeof(line); ++i)
            assert(fwrite(line, 1, sizeof(line), big) == sizeof(line));
        assert(fwrite("b", 1, extra, big) == extra && !fclose(big));
        ed = (struct editor){0};
        assert(editor_open(&ed, path) == !extra);
        assert(extra ? strstr(ed.message, "larger than") != NULL : ed.length == EDITOR_FILE_LIMIT);
        editor_destroy(&ed);
    }
    unlink(alias);
    unlink(path);
    rmdir(directory);
    puts("Editor tests passed");
}
