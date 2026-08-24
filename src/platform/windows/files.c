#include "win32.h"
#include "platform.h"
#include <fcntl.h>
#include <io.h>
#include <stdint.h>
#include <stdlib.h>
#include <wchar.h>

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
        swprintf(temporary + directory_length, 64, L".jot-%08lx-%08lx.tmp",
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
