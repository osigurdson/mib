#define _GNU_SOURCE
#include "platform.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

FILE *platform_file_open_read(const char *path, bool *missing, char *error, size_t error_size)
{
    *missing = false;
    FILE *file = fopen(path, "rb");
    if (!file) {
        if (errno == ENOENT)
            *missing = true;
        else
            snprintf(error, error_size, "Open: %s", strerror(errno));
        return NULL;
    }
    struct stat st;
    if (fstat(fileno(file), &st) < 0 || !S_ISREG(st.st_mode)) {
        snprintf(error, error_size, "Only regular text files are supported");
        fclose(file);
        return NULL;
    }
    return file;
}

bool platform_file_write_atomic(const char *path, const char *data, size_t length, char *error,
                                size_t error_size)
{
    char *target = realpath(path, NULL);
    if (!target)
        target = strdup(path);
    if (!target) {
        snprintf(error, error_size, "Out of memory");
        return false;
    }
    struct stat st;
    bool exists = stat(target, &st) == 0;
    if (exists && !S_ISREG(st.st_mode)) {
        snprintf(error, error_size, "Only regular text files are supported");
        free(target);
        return false;
    }
    char *temporary = NULL;
    int fd = -1;
    if (asprintf(&temporary, "%s.XXXXXX", target) < 0) {
        temporary = NULL;
        errno = ENOMEM;
    } else
        fd = mkstemp(temporary);
    bool ok = fd >= 0;
    if (ok && exists)
        ok = fchmod(fd, st.st_mode & 0777) == 0;
    size_t written = 0;
    while (ok && written < length) {
        size_t count = length - written;
        if (count > 65536)
            count = 65536;
        ssize_t n = write(fd, data + written, count);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            if (!n)
                errno = EIO;
            ok = false;
        } else
            written += (size_t)n;
    }
    if (ok)
        ok = fsync(fd) == 0;
    int saved_error = errno;
    if (fd >= 0 && close(fd) < 0 && ok) {
        saved_error = errno;
        ok = false;
    }
    if (ok && rename(temporary, target) < 0) {
        saved_error = errno;
        ok = false;
    }
    if (!ok) {
        if (temporary && fd >= 0)
            unlink(temporary);
        snprintf(error, error_size, "Write: %s", strerror(saved_error));
    }
    free(target);
    free(temporary);
    return ok;
}

bool platform_file_remove(const char *path) { return unlink(path) == 0 || errno == ENOENT; }
