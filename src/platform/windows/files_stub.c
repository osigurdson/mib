#include "platform.h"

FILE *platform_file_open_read(const char *path, bool *missing, char *error, size_t error_size)
{
    (void)path;
    *missing = false;
    /* TODO: UTF-8 to UTF-16 paths, regular-file check, and binary read stream. */
    snprintf(error, error_size, "Windows file loading is not implemented yet");
    return NULL;
}

bool platform_file_write_atomic(const char *path, const char *data, size_t length,
                                char *error, size_t error_size)
{
    (void)path; (void)data; (void)length;
    /* TODO: sibling temporary file, flush, and atomic replacement via Win32. */
    snprintf(error, error_size, "Windows file saving is not implemented yet");
    return false;
}
