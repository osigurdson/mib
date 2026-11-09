#define _GNU_SOURCE
#include "platform.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

FILE *platform_file_open_read(const char *path, bool *missing, char *error, size_t error_size)
{
    *missing = false;
    /* Open without blocking, so a FIFO with no writer can't hang the UI before fstat rejects
     * it, then restore blocking reads for the regular file. */
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_NOCTTY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT)
            *missing = true;
        else
            snprintf(error, error_size, "Open: %s", strerror(errno));
        return NULL;
    }
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        snprintf(error, error_size, "Only regular text files are supported");
        close(fd);
        return NULL;
    }
    int flags = fcntl(fd, F_GETFL);
    FILE *file = flags < 0 || fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) < 0 ? NULL : fdopen(fd, "rb");
    if (!file) {
        snprintf(error, error_size, "Open: %s", strerror(errno));
        close(fd);
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

static bool plain_name(const char *name)
{
    return *name && strcmp(name, ".") && strcmp(name, "..");
}

bool platform_file_publish(const char *directory, const char *relative, const char *data,
                           size_t length)
{
    char *copy = strdup(relative);
    int folder = copy ? open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC) : -1;
    char *name = copy;
    /* Walk RELATIVE's folders one at a time, refusing links. */
    for (char *slash; folder >= 0 && (slash = strchr(name, '/')); name = slash + 1) {
        *slash = 0;
        int next = plain_name(name)
                       ? openat(folder, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)
                       : -1;
        close(folder);
        folder = next;
    }
    bool ok = folder >= 0 && plain_name(name);
    char temporary[64];
    int fd = -1;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    static unsigned sequence;
    for (unsigned attempt = 0; ok && attempt < 32 && fd < 0; ++attempt) {
        snprintf(temporary, sizeof(temporary), ".mib-%ld-%lx-%x.tmp", (long)getpid(),
                 (unsigned long)now.tv_nsec, ++sequence);
        fd = openat(folder, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0 && errno != EEXIST)
            ok = false;
    }
    ok = ok && fd >= 0;
    for (size_t written = 0; ok && written < length;) {
        ssize_t n = write(fd, data + written, length - written);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            ok = false;
        else
            written += (size_t)n;
    }
    if (fd >= 0 && close(fd) < 0)
        ok = false;
    /* rename replaces whatever is at NAME, a link included, without following it. */
    if (ok)
        ok = renameat(folder, temporary, folder, name) == 0;
    if (!ok && fd >= 0)
        unlinkat(folder, temporary, 0);
    if (folder >= 0)
        close(folder);
    free(copy);
    return ok;
}

char *platform_private_directory(void)
{
    const char *base = getenv("XDG_RUNTIME_DIR");
    if (!base || base[0] != '/')
        base = getenv("TMPDIR");
    if (!base || base[0] != '/')
        base = "/tmp";
    char *path = NULL;
    if (asprintf(&path, "%s/mib-XXXXXX", base) < 0)
        return NULL;
    if (!mkdtemp(path)) { /* mkdtemp makes it 0700 */
        free(path);
        return NULL;
    }
    return path;
}

bool platform_directory_remove(const char *path) { return rmdir(path) == 0 || errno == ENOENT; }

char *platform_find_program(const char *name)
{
    const char *list = getenv("PATH");
    for (const char *p = list; p && *p;) {
        const char *end = strchr(p, ':');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        if (n && p[0] == '/') {
            char *path = NULL;
            if (asprintf(&path, "%.*s/%s", (int)n, p, name) < 0)
                return NULL;
            struct stat st;
            if (!stat(path, &st) && S_ISREG(st.st_mode) && !access(path, X_OK))
                return path;
            free(path);
        }
        p = end ? end + 1 : p + n;
    }
    return NULL;
}

char *platform_getenv(const char *name)
{
    const char *value = getenv(name);
    return value && *value ? strdup(value) : NULL;
}
