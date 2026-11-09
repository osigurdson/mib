#ifndef PLATFORM_H
#define PLATFORM_H
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

struct app;
/* Each build links exactly one window/event backend. Zero means clean exit. */
int platform_run(struct app *app);
/* Tell the user something outside the window, such as usage or a startup error: stdout or
 * stderr on Linux, a message box on Windows, where mib has no console. */
void platform_report(const char *text, bool error);
/* Dedicated zero-filled pages. Release with the original allocation size. */
void *platform_allocate_zeroed(size_t bytes);
void platform_release(void *memory, size_t bytes);
void platform_trim_heap(void);
/* Paths are UTF-8. Missing files are distinguished from errors; caller fclose's. */
FILE *platform_file_open_read(const char *path, bool *missing, char *error, size_t error_size);
/* Replace a regular file only after a successful complete write. */
bool platform_file_write_atomic(const char *path, const char *data, size_t length, char *error,
                                size_t error_size);
/* Remove a file; an already missing file counts as success. */
bool platform_file_remove(const char *path);
/* Write DIRECTORY/RELATIVE in full under a temporary name, then rename it into place. A link
 * already at that name is replaced, not followed, and a link among RELATIVE's folders makes it
 * fail, so a crafted notes folder can't redirect the write. Missing folders aren't created. */
bool platform_file_publish(const char *directory, const char *relative, const char *data,
                           size_t length);
/* A new, empty folder that only this user can use, for a helper process's output, or NULL.
 * The caller frees the path; platform_directory_remove removes the folder once it's empty. */
char *platform_private_directory(void);
bool platform_directory_remove(const char *path);
/* The absolute path of the program NAME in one of PATH's absolute folders, or NULL. Relative
 * and empty entries, which would mean the current folder, are skipped. The caller frees it. */
char *platform_find_program(const char *name);
/* An environment variable as UTF-8, or NULL when unset or empty. The caller frees it. */
char *platform_getenv(const char *name);
#endif
