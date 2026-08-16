#ifndef SESSION_H
#define SESSION_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
struct app;
uint64_t session_now(void);
/* Resolve a jot page, acquire ownership, open its text and restore preferences.
   Return 1 for an owned page, 0 when an existing window was notified, -1 on error. */
int session_open(struct app *app, const char *name);
void session_close(struct app *app);
void session_changed(struct app *app);
void session_tick(struct app *app);
void session_focus(struct app *app, bool focused);
bool session_save(struct app *app);
char *session_absolute(const char *path);
bool session_write_exclusive(const char *path, const char *data, size_t length);
char *session_path(const struct app *app, const char *relative);
bool session_mkdir(const char *path);
char *session_read(const char *path, size_t maximum, size_t *length);
bool session_write_new(const char *path, const char *data, size_t length);
#endif
