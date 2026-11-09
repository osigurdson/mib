#include "win32.h"
#include "platform.h"
#include <fcntl.h>
#include <io.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

static bool regular(HANDLE file, BY_HANDLE_FILE_INFORMATION *info)
{
    if (GetFileType(file) != FILE_TYPE_DISK) {
        SetLastError(ERROR_FILE_INVALID);
        return false;
    }
    if (!GetFileInformationByHandle(file, info))
        return false;
    if (info->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        SetLastError(ERROR_DIRECTORY);
        return false;
    }
    return true;
}

FILE *platform_file_open_read(const char *path, bool *missing, char *error, size_t error_size)
{
    *missing = false;
    wchar_t *wide = windows_path(path);
    HANDLE file = wide ? CreateFileW(wide, GENERIC_READ,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                                     OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN |
                                         FILE_FLAG_BACKUP_SEMANTICS,
                                     NULL)
                       : INVALID_HANDLE_VALUE;
    DWORD code = GetLastError();
    free(wide);
    if (file == INVALID_HANDLE_VALUE) {
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
            *missing = true;
        else
            windows_error(error, error_size, "Open", code);
        return NULL;
    }
    BY_HANDLE_FILE_INFORMATION info;
    if (!regular(file, &info)) {
        code = GetLastError();
        CloseHandle(file);
        windows_error(error, error_size, "Open regular file", code);
        return NULL;
    }
    int fd = _open_osfhandle((intptr_t)file, _O_RDONLY | _O_BINARY);
    if (fd < 0) {
        CloseHandle(file);
        snprintf(error, error_size, "Open: could not create a binary file stream");
        return NULL;
    }
    FILE *stream = _fdopen(fd, "rb");
    if (!stream) {
        _close(fd);
        snprintf(error, error_size, "Open: could not create a binary file stream");
    }
    return stream;
}

bool platform_file_write_atomic(const char *path, const char *data, size_t length, char *error,
                                size_t error_size)
{
    wchar_t *target = windows_path(path), *temporary = NULL;
    HANDLE destination = INVALID_HANDLE_VALUE, output = INVALID_HANDLE_VALUE;
    bool ok = false, exists = false, created = false;
    DWORD code = GetLastError();
    if (!target)
        goto cleanup;
    /* Preflight the read/delete sharing rights ReplaceFileW itself needs. */
    destination = CreateFileW(target, GENERIC_READ | DELETE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                              OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (destination != INVALID_HANDLE_VALUE) {
        BY_HANDLE_FILE_INFORMATION info;
        if (!regular(destination, &info)) {
            code = GetLastError();
            goto cleanup;
        }
        if (info.dwFileAttributes & FILE_ATTRIBUTE_READONLY) {
            code = ERROR_ACCESS_DENIED;
            goto cleanup;
        }
        /* Follow symlinks/reparse points instead of replacing the link itself. */
        DWORD count = GetFinalPathNameByHandleW(destination, NULL, 0, FILE_NAME_NORMALIZED);
        if (!count) {
            code = GetLastError();
            goto cleanup;
        }
        wchar_t *resolved = malloc(((size_t)count + 1) * sizeof(*resolved));
        if (!resolved) {
            code = ERROR_NOT_ENOUGH_MEMORY;
            goto cleanup;
        }
        DWORD used =
            GetFinalPathNameByHandleW(destination, resolved, count + 1, FILE_NAME_NORMALIZED);
        if (!used || used > count) {
            code = used ? ERROR_FILENAME_EXCED_RANGE : GetLastError();
            free(resolved);
            goto cleanup;
        }
        free(target);
        target = resolved;
        CloseHandle(destination);
        destination = INVALID_HANDLE_VALUE;
        exists = true;
    } else {
        code = GetLastError();
        if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND)
            goto cleanup;
        DWORD attributes = GetFileAttributesW(target);
        if (attributes != INVALID_FILE_ATTRIBUTES) {
            code = ERROR_CANT_ACCESS_FILE;
            goto cleanup;
        }
    }
    wchar_t *slash = wcsrchr(target, L'\\');
    if (!slash) {
        code = ERROR_INVALID_NAME;
        goto cleanup;
    }
    size_t directory_length = (size_t)(slash - target) + 1;
    temporary = malloc((directory_length + 64) * sizeof(*temporary));
    if (!temporary) {
        code = ERROR_NOT_ENOUGH_MEMORY;
        goto cleanup;
    }
    wmemcpy(temporary, target, directory_length);
    static volatile LONG sequence;
    for (int attempt = 0; attempt < 32; ++attempt) {
        swprintf(temporary + directory_length, 64, L".mib-%08lx-%08lx.tmp",
                 (unsigned long)GetCurrentProcessId(),
                 (unsigned long)InterlockedIncrement(&sequence));
        output =
            CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (output != INVALID_HANDLE_VALUE) {
            created = true;
            break;
        }
        code = GetLastError();
        if (code != ERROR_FILE_EXISTS && code != ERROR_ALREADY_EXISTS)
            goto cleanup;
    }
    if (output == INVALID_HANDLE_VALUE)
        goto cleanup;
    for (size_t written = 0; written < length;) {
        DWORD count = (DWORD)(length - written > 65536 ? 65536 : length - written), done;
        if (!WriteFile(output, data + written, count, &done, NULL)) {
            code = GetLastError();
            goto cleanup;
        }
        if (!done) {
            code = ERROR_WRITE_FAULT;
            goto cleanup;
        }
        written += done;
    }
    if (!FlushFileBuffers(output)) {
        code = GetLastError();
        goto cleanup;
    }
    if (!CloseHandle(output)) {
        code = GetLastError();
        output = INVALID_HANDLE_VALUE;
        goto cleanup;
    }
    output = INVALID_HANDLE_VALUE;
    if (exists)
        ok = ReplaceFileW(target, temporary, NULL, 0, NULL, NULL) != 0;
    else
        ok = MoveFileExW(temporary, target, MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok)
        code = GetLastError();
cleanup:
    if (destination != INVALID_HANDLE_VALUE)
        CloseHandle(destination);
    if (output != INVALID_HANDLE_VALUE)
        CloseHandle(output);
    if (!ok && created)
        DeleteFileW(temporary);
    if (!ok)
        windows_error(error, error_size, "Write", code);
    free(target);
    free(temporary);
    return ok;
}

bool platform_file_remove(const char *path)
{
    wchar_t *wide = windows_path(path);
    if (!wide)
        return false;
    bool ok = DeleteFileW(wide) != 0;
    DWORD code = GetLastError();
    free(wide);
    return ok || code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND;
}

/* getenv returns the ANSI code page, which loses characters such as those in a user name. */
char *platform_getenv(const char *name)
{
    wchar_t *wide = windows_utf16(name);
    DWORD count = wide ? GetEnvironmentVariableW(wide, NULL, 0) : 0;
    wchar_t *value = count > 1 ? malloc((size_t)count * sizeof(*value)) : NULL;
    char *text = NULL;
    if (value && GetEnvironmentVariableW(wide, value, count) == count - 1)
        text = windows_utf8(value);
    free(value);
    free(wide);
    return text;
}

static bool plain_name(const char *name)
{
    return *name && strcmp(name, ".") && strcmp(name, "..") && !strchr(name, ':');
}

static wchar_t *join_wide(const wchar_t *folder, const wchar_t *name)
{
    size_t n = wcslen(folder) + wcslen(name) + 2;
    wchar_t *path = malloc(n * sizeof(*path));
    if (path)
        swprintf(path, n, L"%ls%ls%ls", folder,
                 *folder && folder[wcslen(folder) - 1] == L'\\' ? L"" : L"\\", name);
    return path;
}

/* Folders opened on the way to a file, held without delete sharing so they can't be swapped
 * for a link while they're in use. */
struct held_folders {
    HANDLE handles[8];
    unsigned count;
};
static void release_folders(struct held_folders *held)
{
    while (held->count)
        CloseHandle(held->handles[--held->count]);
}
/* DIRECTORY/RELATIVE as a wide path, after opening each folder in RELATIVE (which this cuts
 * up) without following reparse points; NULL if one is a link or missing. *missing says
 * whether a folder on the way doesn't exist. */
static wchar_t *walk(const char *directory, char *relative, struct held_folders *held,
                     bool *missing)
{
    char *name = relative;
    wchar_t *folder = windows_path(directory);
    bool ok = folder != NULL;
    *missing = false;
    for (char *slash; ok && (slash = strpbrk(name, "/\\")); name = slash + 1) {
        *slash = 0;
        wchar_t *part = plain_name(name) && held->count < 8 ? windows_utf16(name) : NULL;
        wchar_t *next = part ? join_wide(folder, part) : NULL;
        free(part);
        free(folder);
        folder = next;
        HANDLE h =
            folder ? CreateFileW(folder, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 NULL, OPEN_EXISTING,
                                 FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL)
                   : INVALID_HANDLE_VALUE;
        if (h == INVALID_HANDLE_VALUE) {
            DWORD code = GetLastError();
            *missing = folder && (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND);
        }
        BY_HANDLE_FILE_INFORMATION info;
        ok = h != INVALID_HANDLE_VALUE;
        if (ok) {
            held->handles[held->count++] = h;
            ok = GetFileInformationByHandle(h, &info) &&
                 (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                 !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
        }
    }
    wchar_t *wide_name = ok && plain_name(name) ? windows_utf16(name) : NULL;
    wchar_t *target = wide_name ? join_wide(folder, wide_name) : NULL;
    free(wide_name);
    free(folder);
    return target;
}

static bool write_all(HANDLE output, const char *data, size_t length)
{
    for (size_t written = 0; written < length;) {
        DWORD count = (DWORD)(length - written > 65536 ? 65536 : length - written), done;
        if (!WriteFile(output, data + written, count, &done, NULL) || !done)
            return false;
        written += done;
    }
    return true;
}

bool platform_file_publish(const char *directory, const char *relative, const char *data,
                           size_t length)
{
    struct held_folders held = {0};
    bool missing;
    char *copy = _strdup(relative);
    wchar_t *target = copy ? walk(directory, copy, &held, &missing) : NULL, *temporary = NULL;
    wchar_t *slash = target ? wcsrchr(target, L'\\') : NULL;
    HANDLE output = INVALID_HANDLE_VALUE;
    static volatile LONG sequence;
    for (int attempt = 0; slash && attempt < 32 && output == INVALID_HANDLE_VALUE; ++attempt) {
        wchar_t unique[64];
        swprintf(unique, 64, L".mib-%08lx-%08lx.tmp", (unsigned long)GetCurrentProcessId(),
                 (unsigned long)InterlockedIncrement(&sequence));
        *slash = 0;
        free(temporary);
        temporary = join_wide(target, unique);
        *slash = L'\\';
        output = temporary ? CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                                         FILE_ATTRIBUTE_NORMAL, NULL)
                           : INVALID_HANDLE_VALUE;
        DWORD code = GetLastError();
        if (output == INVALID_HANDLE_VALUE && code != ERROR_FILE_EXISTS &&
            code != ERROR_ALREADY_EXISTS)
            break;
    }
    bool created = output != INVALID_HANDLE_VALUE;
    bool ok = created && write_all(output, data, length);
    if (created && !CloseHandle(output))
        ok = false;
    /* Renaming over a link replaces the link itself. */
    if (ok)
        ok = MoveFileExW(temporary, target, MOVEFILE_REPLACE_EXISTING) != 0;
    if (!ok && created)
        DeleteFileW(temporary);
    release_folders(&held);
    free(temporary);
    free(target);
    free(copy);
    return ok;
}

bool platform_file_create_at(const char *directory, const char *relative, const char *data,
                             size_t length)
{
    struct held_folders held = {0};
    bool missing;
    char *copy = _strdup(relative);
    wchar_t *target = copy ? walk(directory, copy, &held, &missing) : NULL;
    /* CREATE_NEW fails for anything already at the name, a link included. */
    HANDLE output = target ? CreateFileW(target, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                                         FILE_ATTRIBUTE_NORMAL, NULL)
                           : INVALID_HANDLE_VALUE;
    bool created = output != INVALID_HANDLE_VALUE;
    bool ok = created && write_all(output, data, length);
    if (created && !CloseHandle(output))
        ok = false;
    if (!ok && created)
        DeleteFileW(target);
    release_folders(&held);
    free(target);
    free(copy);
    return ok;
}

bool platform_file_remove_at(const char *directory, const char *relative)
{
    struct held_folders held = {0};
    bool missing = false;
    char *copy = _strdup(relative);
    wchar_t *target = copy ? walk(directory, copy, &held, &missing) : NULL;
    /* DeleteFileW removes a link itself, not what it points to. */
    bool ok = target ? DeleteFileW(target) != 0 : missing;
    DWORD code = GetLastError();
    if (target && !ok)
        ok = code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND;
    release_folders(&held);
    free(target);
    free(copy);
    return ok;
}

char *platform_private_directory(void)
{
    wchar_t temp[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH + 1, temp);
    if (!n || n > MAX_PATH)
        return NULL;
    static volatile LONG sequence;
    for (int attempt = 0; attempt < 32; ++attempt) {
        wchar_t path[MAX_PATH + 64];
        swprintf(path, MAX_PATH + 64, L"%lsmib-%08lx-%08lx-%08lx", temp,
                 (unsigned long)GetCurrentProcessId(), (unsigned long)GetTickCount(),
                 (unsigned long)InterlockedIncrement(&sequence));
        /* The user's temporary folder is private to them, and the new folder inherits that. */
        if (CreateDirectoryW(path, NULL))
            return windows_utf8(path);
        if (GetLastError() != ERROR_ALREADY_EXISTS)
            return NULL;
    }
    return NULL;
}

bool platform_directory_remove(const char *path)
{
    wchar_t *wide = windows_path(path);
    if (!wide)
        return false;
    bool ok = RemoveDirectoryW(wide) != 0;
    DWORD code = GetLastError();
    free(wide);
    return ok || code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND;
}

char *platform_find_program(const char *name)
{
    DWORD count = GetEnvironmentVariableW(L"PATH", NULL, 0);
    wchar_t *list = count > 1 ? malloc((size_t)count * sizeof(*list)) : NULL;
    wchar_t *wide_name = windows_utf16(name);
    char *found = NULL;
    if (list && wide_name && GetEnvironmentVariableW(L"PATH", list, count) == count - 1) {
        for (wchar_t *entry = list, *end; entry && !found; entry = end ? end + 1 : NULL) {
            if ((end = wcschr(entry, L';')))
                *end = 0;
            size_t n = wcslen(entry);
            if (n >= 2 && entry[0] == L'"' && entry[n - 1] == L'"') {
                entry[n - 1] = 0;
                ++entry;
            }
            bool drive =
                iswalpha(entry[0]) && entry[1] == L':' && (entry[2] == L'\\' || entry[2] == L'/');
            bool unc = entry[0] == L'\\' && entry[1] == L'\\';
            if (!drive && !unc)
                continue;
            wchar_t *path = join_wide(entry, wide_name);
            DWORD attributes = path ? GetFileAttributesW(path) : INVALID_FILE_ATTRIBUTES;
            if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY))
                found = windows_utf8(path);
            free(path);
        }
    }
    free(wide_name);
    free(list);
    return found;
}
