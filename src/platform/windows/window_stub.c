#include "platform.h"
#include <stdlib.h>

int platform_run(struct app *app)
{
    (void)app;
    /* TODO: Win32 window/event loop, normalized app_input, DPI updates, and
       top-down 32-bit DIB presentation of app_paint's CPU pixels. */
    fprintf(stderr, "Windows window backend is not implemented yet.\n");
    return EXIT_FAILURE;
}
