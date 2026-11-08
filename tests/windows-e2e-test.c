#include "../src/platform/windows/win32.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

struct launched {
    PROCESS_INFORMATION process;
    HWND window;
};
static BOOL CALLBACK find_window(HWND window, LPARAM param)
{
    struct launched *app = (void *)param;
    DWORD pid;
    GetWindowThreadProcessId(window, &pid);
    wchar_t klass[64];
    GetClassNameW(window, klass, 64);
    if (pid == app->process.dwProcessId && !wcscmp(klass, L"MibWindow"))
        app->window = window;
    return TRUE;
}
/* Start mib.exe on the notes folder; it reads MIB_DIR from the inherited environment. */
static struct launched launch(const wchar_t *exe, const wchar_t *notes)
{
    assert(SetEnvironmentVariableW(L"MIB_DIR", notes));
    wchar_t command[4096];
    swprintf(command, 4096, L"\"%ls\"", exe);
    STARTUPINFOW startup = {0};
    startup.cb = sizeof(startup);
    struct launched app = {0};
    assert(CreateProcessW(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &app.process));
    DWORD started = GetTickCount();
    while (!app.window && GetTickCount() - started < 10000) {
        EnumWindows(find_window, (LPARAM)&app);
        if (WaitForSingleObject(app.process.hProcess, 0) != WAIT_TIMEOUT)
            break;
        Sleep(20);
    }
    assert(app.window);
    return app;
}
static void type(struct launched *app, const wchar_t *text)
{
    while (*text) {
        DWORD_PTR result;
        assert(
            SendMessageTimeoutW(app->window, WM_CHAR, *text++, 1, SMTO_ABORTIFHUNG, 5000, &result));
    }
}
static void exited(struct launched *app)
{
    assert(WaitForSingleObject(app->process.hProcess, 5000) == WAIT_OBJECT_0);
    DWORD code;
    assert(GetExitCodeProcess(app->process.hProcess, &code) && code == 0);
    CloseHandle(app->process.hThread);
    CloseHandle(app->process.hProcess);
}
static void expect(const wchar_t *path, const char *bytes)
{
    FILE *file = _wfopen(path, L"rb");
    assert(file);
    char content[512] = {0};
    size_t length = strlen(bytes);
    assert(fread(content, 1, sizeof(content), file) == length);
    assert(!memcmp(content, bytes, length));
    fclose(file);
}
/* Remove a folder and everything in it. */
static void remove_tree(const wchar_t *folder)
{
    wchar_t pattern[1200];
    swprintf(pattern, 1200, L"%ls\\*", folder);
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW(pattern, &found);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(found.cFileName, L".") || !wcscmp(found.cFileName, L".."))
                continue;
            wchar_t path[1200];
            swprintf(path, 1200, L"%ls\\%ls", folder, found.cFileName);
            if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                remove_tree(path);
            else
                assert(DeleteFileW(path));
        } while (FindNextFileW(search, &found));
        FindClose(search);
    }
    assert(RemoveDirectoryW(folder));
}

int main(void)
{
    wchar_t exe[2048], temp[MAX_PATH], root[1024], notes[2048], page[2048], other[2048],
        other_page[2048];
    assert(GetModuleFileNameW(NULL, exe, 2048));
    wcscpy(wcsrchr(exe, L'\\') + 1, L"mib.exe");
    assert(GetTempPathW(MAX_PATH, temp));
    swprintf(root, 1024, L"%lsmib-e2e-%lu-%lu", temp, (unsigned long)GetCurrentProcessId(),
             (unsigned long)GetTickCount());
    assert(CreateDirectoryW(root, NULL));
    /* mib inherits these, keeping its settings and positions out of the Wine profile. */
    wchar_t config[1100], state[1100];
    swprintf(config, 1100, L"%ls\\config", root);
    swprintf(state, 1100, L"%ls\\state", root);
    assert(SetEnvironmentVariableW(L"APPDATA", config));
    assert(SetEnvironmentVariableW(L"LOCALAPPDATA", state));
    swprintf(notes, 2048, L"%ls\\caf\u00e9 \u6587\u4ef6 \U0001f4dd", root);
    swprintf(page, 2048, L"%ls\\1\\page.md", notes);
    struct launched app = launch(exe, notes);
    type(&app, L"ihello\rworld\x1b:w\r");
    expect(page, "hello\nworld");
    type(&app, L":q\r");
    exited(&app);

    /* Reopening restores the cursor, so gg goes back to the start. */
    app = launch(exe, notes);
    type(&app, L"ggiX\x1b:q\r");
    assert(WaitForSingleObject(app.process.hProcess, 150) == WAIT_TIMEOUT);
    SendMessageW(app.window, WM_CLOSE, 0, 0);
    assert(WaitForSingleObject(app.process.hProcess, 150) == WAIT_TIMEOUT);
    type(&app, L":wq\r");
    exited(&app);
    expect(page, "Xhello\nworld");

    swprintf(other, 2048, L"%ls\\unicode \U0001f4dd", root);
    swprintf(other_page, 2048, L"%ls\\1\\page.md", other);
    app = launch(exe, other);
    type(&app, L"iUTF-8: \u00e9 \U0001f4dd\x1b:wq\r");
    exited(&app);
    expect(other_page, "UTF-8: \xc3\xa9 \xf0\x9f\x93\x9d");

    /* A page edited outside mib with Windows CRLF line ends keeps them on save. */
    FILE *dos = _wfopen(page, L"wb");
    assert(dos);
    assert(fwrite("a\r\nb\r\n", 1, 6, dos) == 6);
    fclose(dos);
    app = launch(exe, notes);
    type(&app, L"ggiX\x1b:wq\r");
    exited(&app);
    expect(page, "Xa\r\nb\r\n");
    remove_tree(root);
    puts("Real Windows executable: Unicode notes folders, load/edit/:w/:q/:wq, close protection, "
         "UTF-16 text and CRLF passed");
}
