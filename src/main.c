#include "app.h"
#include "platform.h"
#include "session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int app_main(int argc, char **argv)
{
    const char *name = NULL;
    bool file = false;
    if (argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
        printf("Usage: %s [page-name | path/to/page | file.txt]\n       %s --page folder\n       "
               "%s --file file\nNo argument opens $JOT_DIR or ~/Documents/jot/default (~/jot "
               "without Documents).\nPages autosave; explicit files use :w. Use --page for a "
               "folder containing a dot.\n",
               argv[0], argv[0], argv[0]);
        return EXIT_SUCCESS;
    }
    if (argc == 3 && (!strcmp(argv[1], "--file") || !strcmp(argv[1], "--page"))) {
        file = !strcmp(argv[1], "--file");
        name = argv[2];
    } else if (argc == 2) {
        name = argv[1];
        const char *base = strrchr(name, '/');
        if (!base)
            base = strrchr(name, '\\');
        base = base ? base + 1 : name;
        file = strchr(base, '.') && strcmp(base, ".") && strcmp(base, "..");
    } else if (argc > 1) {
        fprintf(stderr, "Usage: %s [page | --file file | --page folder]\n", argv[0]);
        return EXIT_FAILURE;
    }
    struct app app;
    if (!app_open(&app, file ? name : NULL))
        goto failed;
    if (!file) {
        int opened = session_open(&app, name);
        if (!opened) {
            app_close(&app);
            return EXIT_SUCCESS;
        }
        if (opened < 0)
            goto failed;
    }
    int status = platform_run(&app);
    app_close(&app);
    return status;
failed:
    fprintf(stderr, "%s\n", app.editor.message[0] ? app.editor.message : "Could not open page");
    app_close(&app);
    return EXIT_FAILURE;
}
