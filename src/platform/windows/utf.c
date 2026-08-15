#include "win32.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

wchar_t *windows_utf16(const char *text)
{
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, NULL, 0);
    if (!count) return NULL;
    wchar_t *wide = malloc((size_t)count * sizeof(*wide));
    if (!wide) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide, count)) {
        DWORD error = GetLastError(); free(wide); SetLastError(error); return NULL;
    }
    return wide;
}

char *windows_utf8(const wchar_t *text)
{
    int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, NULL, 0, NULL, NULL);
    if (!count) return NULL;
    char *utf8 = malloc((size_t)count);
    if (!utf8) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, utf8, count, NULL, NULL)) {
        DWORD error = GetLastError(); free(utf8); SetLastError(error); return NULL;
    }
    return utf8;
}

wchar_t *windows_path(const char *utf8)
{
    wchar_t *wide = windows_utf16(utf8);
    if (!wide) return NULL;
    /* Resolve relative paths before adding the extended-length prefix. */
    for (wchar_t *p = wide; *p; ++p) if (*p == L'/') *p = L'\\';
    DWORD count = GetFullPathNameW(wide, 0, NULL, NULL);
    wchar_t *full = count ? malloc((size_t)count * sizeof(*full)) : NULL;
    if (!full) {
        DWORD error = count ? ERROR_NOT_ENOUGH_MEMORY : GetLastError();
        free(wide); SetLastError(error); return NULL;
    }
    DWORD used = GetFullPathNameW(wide, count, full, NULL);
    free(wide);
    if (!used || used >= count) {
        DWORD error = used ? ERROR_FILENAME_EXCED_RANGE : GetLastError();
        free(full); SetLastError(error); return NULL;
    }
    if (!wcsncmp(full, L"\\\\?\\", 4)) return full;
    bool unc = full[0] == L'\\' && full[1] == L'\\';
    size_t length = wcslen(full);
    wchar_t *path = malloc((length + 9) * sizeof(*path));
    if (!path) { free(full); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    wcscpy(path, unc ? L"\\\\?\\UNC\\" : L"\\\\?\\");
    wcscat(path, full + (unc ? 2 : 0));
    free(full);
    return path;
}

void windows_error(char *error, size_t size, const char *operation, DWORD code)
{
    wchar_t message[256] = {0};
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, code, 0, message, 256, NULL);
    size_t length = wcslen(message);
    while (length && (message[length - 1] == L'\r' || message[length - 1] == L'\n'))
        message[--length] = 0;
    char *text = windows_utf8(message);
    snprintf(error, size, "%s: %s (Windows error %lu)", operation,
             text && *text ? text : "Operation failed", (unsigned long)code);
    free(text);
}
