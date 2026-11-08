#ifndef MIB_WIN32_H
#define MIB_WIN32_H
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <stddef.h>

wchar_t *windows_utf16(const char *text);
char *windows_utf8(const wchar_t *text);
wchar_t *windows_path(const char *utf8);
void windows_error(char *error, size_t size, const char *operation, DWORD code);
#endif
