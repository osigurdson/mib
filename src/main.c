#include "app.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>

int app_main(int argc, char **argv)
{
    if (argc > 2) {
        fprintf(stderr, "Usage: %s [file]\n", argv[0]);
        return EXIT_FAILURE;
    }
    struct app app;
    if (!app_open(&app, argc == 2 ? argv[1] : NULL)) {
        fprintf(stderr, "%s\n", app.editor.message[0] ? app.editor.message : "Could not open file");
        app_close(&app);
        return EXIT_FAILURE;
    }
    int status = platform_run(&app);
    app_close(&app);
    return status;
}
