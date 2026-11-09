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

static void make_folder(const char *path)
{
    wchar_t *w = windows_path(path);
    assert(w && CreateDirectoryW(w, NULL));
    free(w);
}
static void remove_folder(const char *path)
{
    wchar_t *w = windows_path(path);
    assert(w && RemoveDirectoryW(w));
    free(w);
}
/* What OCR relies on: tesseract.exe only from PATH's absolute folders, never the current
 * folder; a private output folder; and results put in place by rename. */
static void ocr_helpers_test(const wchar_t *directory)
{
    char *root = windows_utf8(directory), bin[1200], program[1300], page[1200], assets[1300],
         sidecar[1400], victim[1300], error[512];
    assert(root);
    snprintf(bin, sizeof(bin), "%s\\bin", root);
    snprintf(program, sizeof(program), "%s\\mib-test.exe", bin);
    make_folder(bin);
    assert(platform_file_write_atomic(program, "MZ", 2, error, sizeof(error)));
    wchar_t previous[32768], cwd[1024];
    DWORD had_path = GetEnvironmentVariableW(L"PATH", previous, 32768);
    assert(GetCurrentDirectoryW(1024, cwd));
    wchar_t *wide_bin = windows_utf16(bin);
    assert(wide_bin && SetCurrentDirectoryW(wide_bin));
    assert(SetEnvironmentVariableW(L"PATH", L".;bin;;\".\";.\\"));
    assert(!platform_find_program("mib-test.exe"));
    wchar_t list[2048];
    swprintf(list, 2048, L".;C:\\mib-nonexistent;\"%ls\"", wide_bin);
    assert(SetEnvironmentVariableW(L"PATH", list));
    char *found = platform_find_program("mib-test.exe");
    assert(found && !strcmp(found, program));
    free(found);
    assert(SetCurrentDirectoryW(cwd));
    SetEnvironmentVariableW(L"PATH", had_path ? previous : NULL);
    free(wide_bin);

    char *output = platform_private_directory();
    assert(output);
    wchar_t *wide_output = windows_path(output);
    DWORD attributes = GetFileAttributesW(wide_output);
    assert(attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY));
    assert(platform_directory_remove(output));
    assert(GetFileAttributesW(wide_output) == INVALID_FILE_ATTRIBUTES);
    free(wide_output);
    free(output);

    snprintf(page, sizeof(page), "%s\\page", root);
    snprintf(assets, sizeof(assets), "%s\\assets", page);
    snprintf(sidecar, sizeof(sidecar), "%s\\probe.png.txt", assets);
    snprintf(victim, sizeof(victim), "%s\\victim.txt", root);
    make_folder(page);
    make_folder(assets);
    assert(platform_file_publish(page, "assets/probe.png.txt", "ocr", 3));
    expect(sidecar, "ocr", 3);
    assert(platform_file_publish(page, "assets\\probe.png.txt", "again", 5));
    expect(sidecar, "again", 5);
    assert(!platform_file_publish(page, "missing/probe.png.txt", "x", 1));
    assert(!platform_file_publish(page, "assets/../probe.png.txt", "x", 1));
    assert(!platform_file_publish(page, "assets/probe.png:stream", "x", 1));
    /* A link planted at the sidecar's name is replaced, not written through. */
    assert(platform_file_write_atomic(victim, "DO NOT OVERWRITE", 16, error, sizeof(error)));
    assert(platform_file_remove(sidecar));
    wchar_t *wide_link = windows_path(sidecar), *wide_victim = windows_path(victim);
    if (CreateSymbolicLinkW(wide_link, wide_victim, SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
        assert(platform_file_publish(page, "assets/probe.png.txt", "ocr", 3));
        expect(victim, "DO NOT OVERWRITE", 16);
        /* Wine emulates symlinks with its own reparse files and can't open the renamed
           result, so only real Windows checks what replaced the link. */
        if (!GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version")) {
            expect(sidecar, "ocr", 3);
            assert(!(GetFileAttributesW(wide_link) & FILE_ATTRIBUTE_REPARSE_POINT));
        }
    } else
        puts("Symbolic links unavailable here; skipped the planted-link publish check");
    free(wide_link);
    free(wide_victim);
    assert(platform_file_remove(sidecar) && platform_file_remove(victim) &&
           platform_file_remove(program));
    remove_folder(assets);
    remove_folder(page);
    remove_folder(bin);
    free(root);
}

int main(void)
{
    wchar_t temp[MAX_PATH], directory[1024], path[1024], collision[1024];
    assert(GetTempPathW(MAX_PATH, temp));
    swprintf(directory, 1024, L"%lsmib-files-%lu-%lu", temp, (unsigned long)GetCurrentProcessId(),
             (unsigned long)GetTickCount());
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
        swprintf(collision, 1024, L"%ls\\.mib-%08lx-%08lx.tmp", directory,
                 (unsigned long)GetCurrentProcessId(), (unsigned long)i);
        HANDLE file = CreateFileW(collision, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
        assert(file != INVALID_HANDLE_VALUE);
        CloseHandle(file);
    }
    assert(!platform_file_write_atomic(utf8, "new", 3, error, sizeof(error)));
    for (unsigned i = 1; i <= 32; ++i) {
        swprintf(collision, 1024, L"%ls\\.mib-%08lx-%08lx.tmp", directory,
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
    swprintf(path, 1024, L"%ls\\.mib-*", directory);
    WIN32_FIND_DATAW found;
    HANDLE scan = FindFirstFileW(path, &found);
    assert(scan == INVALID_HANDLE_VALUE);
    free(root);
    /* Last: it uses platform_file_write_atomic, whose temporary names the collision check
       above counts on starting from 1. */
    ocr_helpers_test(directory);
    assert(RemoveDirectoryW(directory));
    puts("Windows Unicode/long paths, atomic save, collisions, read-only/locked failures, and "
         "regular-file checks passed");
}
