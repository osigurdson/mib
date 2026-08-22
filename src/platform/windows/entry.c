#include "win32.h"
#include "app.h"
#include <stdio.h>
#include <stdlib.h>

int wmain(int argc, wchar_t **wide_argv)
{
    char **argv = calloc((size_t)argc + 1, sizeof(*argv));
    if (!argv)
        return EXIT_FAILURE;
    int status = EXIT_FAILURE;
    for (int i = 0; i < argc; ++i) {
        argv[i] = windows_utf8(wide_argv[i]);
        if (!argv[i]) {
            fprintf(stderr, "Could not decode Unicode command line\n");
            goto cleanup;
        }
    }
    status = app_main(argc, argv);
cleanup:
    for (int i = 0; i < argc; ++i)
        free(argv[i]);
    free(argv);
    return status;
}
