#include "../src/platform/windows/win32.h"
#include "app.h"
#include "platform.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static void expect(const char *path, const char *bytes, size_t length)
{
    char error[512];
    bool missing = false;
    FILE *file = platform_file_open_read(path, &missing, error, sizeof(error));
    assert(file && !missing);
    char *read = malloc(length + 1);
    assert(read);
    assert(fread(read, 1, length + 1, file) == length);
    assert(!memcmp(bytes, read, length));
    free(read);
    fclose(file);
}

int main(void)
{
    wchar_t temp[MAX_PATH], directory[1024], path[1024], collision[1024];
    assert(GetTempPathW(MAX_PATH, temp));
    swprintf(directory, 1024, L"%lstinyvi-files-%lu-%lu", temp,
             (unsigned long)GetCurrentProcessId(), (unsigned long)GetTickCount());
    assert(CreateDirectoryW(directory, NULL));
    swprintf(path, 1024, L"%ls\\caf\u00e9-\u6587\u4ef6-\U0001f4dd.txt", directory);
    char *utf8 = windows_utf8(path);
    assert(utf8);
    wchar_t *roundtrip = windows_utf16(utf8);
    assert(roundtrip && !wcscmp(roundtrip, path));
    free(roundtrip);
    assert(!windows_utf16("\xff"));
    char error[512];
    bool missing;
    assert(!platform_file_open_read(utf8, &missing, error, sizeof(error)) && missing);

    /* A failed CREATE_NEW must never delete somebody else's temporary file. */
    for (unsigned i = 1; i <= 32; ++i) {
        swprintf(collision, 1024, L"%ls\\.tiny-vi-%08lx-%08lx.tmp", directory,
                 (unsigned long)GetCurrentProcessId(), (unsigned long)i);
        HANDLE file = CreateFileW(collision, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
        assert(file != INVALID_HANDLE_VALUE);
        CloseHandle(file);
    }
    assert(!platform_file_write_atomic(utf8, "new", 3, error, sizeof(error)));
    for (unsigned i = 1; i <= 32; ++i) {
        swprintf(collision, 1024, L"%ls\\.tiny-vi-%08lx-%08lx.tmp", directory,
                 (unsigned long)GetCurrentProcessId(), (unsigned long)i);
        assert(GetFileAttributesW(collision) != INVALID_FILE_ATTRIBUTES);
        assert(DeleteFileW(collision));
    }
    const char initial[] = "hello\r\nWindows \xc3\xa9\n";
    assert(platform_file_write_atomic(utf8, initial, sizeof(initial) - 1, error, sizeof(error)));
    expect(utf8, initial, sizeof(initial) - 1);
    struct editor dos = {0};
    assert(editor_open(&dos, utf8));
    assert(dos.crlf && !strchr(dos.text, '\r'));
    editor_input(&dos, KEY_TEXT, ":");
    editor_input(&dos, KEY_TEXT, "w");
    editor_input(&dos, KEY_ENTER, "");
    assert(!dos.modified);
    expect(utf8, "hello\r\nWindows \xc3\xa9\r\n", 19);
    editor_destroy(&dos);
    assert(platform_file_write_atomic(utf8, "replaced", 8, error, sizeof(error)));
    expect(utf8, "replaced", 8);
    assert(SetFileAttributesW(path, FILE_ATTRIBUTE_READONLY));
    assert(!platform_file_write_atomic(utf8, "bad", 3, error, sizeof(error)));
    expect(utf8, "replaced", 8);
    assert(SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL));
    HANDLE locked = CreateFileW(path, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    assert(locked != INVALID_HANDLE_VALUE);
    assert(!platform_file_write_atomic(utf8, "bad", 3, error, sizeof(error)));
    CloseHandle(locked);
    expect(utf8, "replaced", 8);

    struct editor ed = {0};
    assert(editor_open(&ed, utf8));
    editor_input(&ed, KEY_TEXT, "i");
    editor_input(&ed, KEY_TEXT, "X");
    editor_input(&ed, KEY_ESCAPE, "");
    assert(SetFileAttributesW(path, FILE_ATTRIBUTE_READONLY));
    editor_input(&ed, KEY_TEXT, ":");
    editor_input(&ed, KEY_TEXT, "wq");
    editor_input(&ed, KEY_ENTER, "");
    assert(ed.modified && !ed.quit && strstr(ed.message, "Write:"));
    assert(SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL));
    editor_destroy(&ed);
    char *root = windows_utf8(directory);
    assert(root);
    assert(!platform_file_open_read(root, &missing, error, sizeof(error)) && !missing);
    assert(!platform_file_write_atomic(root, "bad", 3, error, sizeof(error)));
    assert(!platform_file_open_read("\xff", &missing, error, sizeof(error)) && !missing);

    char *large = malloc(200000);
    assert(large);
    memset(large, 'x', 200000);
    assert(platform_file_write_atomic(utf8, large, 200000, error, sizeof(error)));
    expect(utf8, large, 200000);
    free(large);
    assert(DeleteFileW(path));
    free(utf8);

    /* Explicit extended-length paths work without changing the user's registry. */
    size_t end = wcslen(directory);
    for (int i = 0; i < 6; ++i) {
        wcscat(directory, L"\\abcdefghijklmnopqrstuvwxabcdefghijklmnopqrstuvwx");
        char *name = windows_utf8(directory);
        assert(name);
        wchar_t *extended = windows_path(name);
        assert(extended);
        assert(CreateDirectoryW(extended, NULL));
        free(extended);
        free(name);
    }
    swprintf(path, 1024, L"%ls\\long.txt", directory);
    utf8 = windows_utf8(path);
    assert(utf8 && strlen(utf8) > 260);
    assert(platform_file_write_atomic(utf8, "long", 4, error, sizeof(error)));
    expect(utf8, "long", 4);
    bool long_replaced = platform_file_write_atomic(utf8, "longer", 6, error, sizeof(error));
    if (long_replaced)
        expect(utf8, "longer", 6);
    else {
        /* Wine 11.21's ReplaceFileW uses a MAX_PATH-sized internal buffer.
           Test the documented failure path without adding Wine-specific I/O. */
        assert(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version"));
        assert(strstr(error, "Windows error 3"));
        expect(utf8, "long", 4);
        puts("Wine limitation: ReplaceFileW on an existing >260-character path fails safely");
    }
    wchar_t *extended = windows_path(utf8);
    assert(extended && DeleteFileW(extended));
    free(extended);
    free(utf8);
    for (int i = 0; i < 6; ++i) {
        char *name = windows_utf8(directory);
        extended = windows_path(name);
        assert(extended);
        assert(RemoveDirectoryW(extended));
        free(extended);
        free(name);
        *wcsrchr(directory, L'\\') = 0;
    }
    assert(wcslen(directory) == end);
    swprintf(path, 1024, L"%ls\\.tiny-vi-*", directory);
    WIN32_FIND_DATAW found;
    HANDLE scan = FindFirstFileW(path, &found);
    assert(scan == INVALID_HANDLE_VALUE);
    free(root);
    assert(RemoveDirectoryW(directory));
    puts("Windows Unicode/long paths, atomic save, collisions, read-only/locked failures, and "
         "regular-file checks passed");
}
