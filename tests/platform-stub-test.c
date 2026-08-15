#include "app.h"
#include "platform.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    struct app app;
    assert(app_open(&app, NULL));
    app_input(&app, KEY_TEXT, "i");
    app_input(&app, KEY_TEXT, "keep this data");
    app_input(&app, KEY_ESCAPE, "");
    app_input(&app, KEY_TEXT, ":");
    app_input(&app, KEY_TEXT, "wq cannot-write.txt");
    app_input(&app, KEY_ENTER, "");
    assert(app.editor.modified && app.running && !app.editor.quit);
    assert(strstr(app.editor.message, "not implemented"));
    assert(!strcmp(app.editor.text, "keep this data"));
    assert(platform_run(&app) != 0);
    app_close(&app);
    assert(!app_open(&app, "cannot-read.txt"));
    assert(strstr(app.editor.message, "not implemented"));
    app_close(&app);
    puts("Windows file/window stubs fail explicitly and preserve unsaved data");
}
