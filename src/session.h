#ifndef SESSION_H
#define SESSION_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
struct app;
uint64_t session_now(void);
/* Open the notes folder holding pages 1/ to 9/: name is a page number to show first, a
   folder path, or NULL for $JOT_DIR or ~/Documents/jot. Acquire ownership of the folder,
   open the page's text and restore preferences. Return 1 for an owned folder, 0 when an
   existing window was notified, -1 on error. */
int session_open(struct app *app, const char *name);
/* 1 to 9 when name is a single page digit, otherwise 0. */
unsigned session_page_number(const char *name);
void session_close(struct app *app);
/* The config folder: $XDG_CONFIG_HOME/jot or ~/.config/jot, or %APPDATA%\jot on Windows. */
char *session_config_directory(void);
/* Apply the shared settings file; session_open does this for pages. On first use, a page's
   old per-page settings seed the file. Closing writes back the settings this window changed. */
void session_load_settings(struct app *app);
void session_focus(struct app *app, bool focused);
char *session_absolute(const char *path);
bool session_write_exclusive(const char *path, const char *data, size_t length);
char *session_path(const struct app *app, const char *relative);
bool session_mkdir(const char *path);
char *session_read(const char *path, size_t maximum, size_t *length);
bool session_write_new(const char *path, const char *data, size_t length);
#endif
