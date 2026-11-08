#include "app.h"
#include "platform.h"
#include "session.h"
#include <stdlib.h>
#include <string.h>
int app_main(int argc, char **argv)
{
    if (argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
        platform_report("mib: Markdown under a mebibyte.\n\nUsage: mib\n"
                        "Opens the nine pages, 1/ to 9/, in $MIB_DIR or ~/Documents/mib (~/mib "
                        "without\nDocuments), on the page shown when it last closed. Alt+1 to "
                        "Alt+9 switch pages.\nSave with :w or Ctrl+S.",
                        false);
        return EXIT_SUCCESS;
    }
    if (argc > 1) {
        platform_report("Usage: mib", true);
        return EXIT_FAILURE;
    }
    struct app app;
    if (!app_open(&app) || session_open(&app, NULL) < 0) {
        platform_report(app.editor.message[0] ? app.editor.message : "Could not open page", true);
        app_close(&app);
        return EXIT_FAILURE;
    }
    int status = platform_run(&app);
    app_close(&app);
    return status;
}
