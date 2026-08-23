#include "app.h"
#include "platform.h"
#include "session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int app_main(int argc, char **argv)
{
    const char *file = argc == 2 ? argv[1] : NULL;
    if (file && (!strcmp(file, "--help") || !strcmp(file, "-h"))) {
        printf("Usage: %s [file]\nWithout a file, Jot opens its nine pages, 1/ to 9/, in $JOT_DIR "
               "or ~/Documents/jot\n(~/jot without Documents), on the page shown when it last "
               "closed. Alt+1 to Alt+9\nswitch pages. Save with :w or Ctrl+S.\n",
               argv[0]);
        return EXIT_SUCCESS;
    }
    if (argc > 2 || (file && file[0] == '-')) {
        fprintf(stderr, "Usage: %s [file]\n", argv[0]);
        return EXIT_FAILURE;
    }
    struct app app;
    if (!app_open(&app, file))
        goto failed;
    if (file)
        session_load_settings(&app);
    else if (session_open(&app, NULL) < 0)
        goto failed;
    int status = platform_run(&app);
    app_close(&app);
    return status;
failed:
    fprintf(stderr, "%s\n", app.editor.message[0] ? app.editor.message : "Could not open page");
    app_close(&app);
    return EXIT_FAILURE;
}
