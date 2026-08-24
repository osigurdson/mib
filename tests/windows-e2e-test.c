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
    if (pid == app->process.dwProcessId && !wcscmp(klass, L"JotWindow"))
        app->window = window;
    return TRUE;
}
static struct launched launch(const wchar_t *exe, const wchar_t *file)
{
    wchar_t command[4096];
    swprintf(command, 4096, L"\"%ls\" \"%ls\"", exe, file);
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

int main(void)
{
    wchar_t exe[2048], temp[MAX_PATH], root[1024], path[2048], second[2048];
    assert(GetModuleFileNameW(NULL, exe, 2048));
    wcscpy(wcsrchr(exe, L'\\') + 1, L"jot.exe");
    assert(GetTempPathW(MAX_PATH, temp));
    swprintf(root, 1024, L"%lsjot-e2e-%lu-%lu", temp, (unsigned long)GetCurrentProcessId(),
             (unsigned long)GetTickCount());
    assert(CreateDirectoryW(root, NULL));
    /* Jot inherits these, keeping its settings and positions out of the Wine profile. */
    wchar_t config[1100], state[1100];
    swprintf(config, 1100, L"%ls\\config", root);
    swprintf(state, 1100, L"%ls\\state", root);
    assert(SetEnvironmentVariableW(L"APPDATA", config));
    assert(SetEnvironmentVariableW(L"LOCALAPPDATA", state));
    swprintf(path, 2048, L"%ls\\caf\u00e9 \u6587\u4ef6 \U0001f4dd.txt", root);
    struct launched app = launch(exe, path);
    type(&app, L"ihello\rworld\x1b:w\r");
    expect(path, "hello\nworld");
    type(&app, L":q\r");
    exited(&app);

    /* Reopening restores the cursor, so gg goes back to the start. */
    app = launch(exe, path);
    type(&app, L"ggiX\x1b:q\r");
    assert(WaitForSingleObject(app.process.hProcess, 150) == WAIT_TIMEOUT);
    SendMessageW(app.window, WM_CLOSE, 0, 0);
    assert(WaitForSingleObject(app.process.hProcess, 150) == WAIT_TIMEOUT);
    type(&app, L":wq\r");
    exited(&app);
    expect(path, "Xhello\nworld");

    swprintf(second, 2048, L"%ls\\unicode \U0001f4dd.txt", root);
    app = launch(exe, second);
    type(&app, L"iUTF-8: \u00e9 \U0001f4dd\x1b:wq\r");
    exited(&app);
    expect(second, "UTF-8: \xc3\xa9 \xf0\x9f\x93\x9d");
    assert(DeleteFileW(second));

    /* The real CLI and editor preserve a Windows CRLF file on save. */
    FILE *dos = _wfopen(path, L"wb");
    assert(dos);
    assert(fwrite("a\r\nb\r\n", 1, 6, dos) == 6);
    fclose(dos);
    app = launch(exe, path);
    type(&app, L"iX\x1b:wq\r");
    exited(&app);
    expect(path, "Xa\r\nb\r\n");
    assert(DeleteFileW(path));
    const wchar_t *made[] = {L"state\\jot\\positions", L"config\\jot\\config"};
    for (unsigned i = 0; i < 2; ++i) {
        wchar_t file[1200];
        swprintf(file, 1200, L"%ls\\%ls", root, made[i]);
        DeleteFileW(file);
    }
    const wchar_t *folders[] = {L"state\\jot", L"state", L"config\\jot", L"config"};
    for (unsigned i = 0; i < 4; ++i) {
        wchar_t folder[1200];
        swprintf(folder, 1200, L"%ls\\%ls", root, folders[i]);
        RemoveDirectoryW(folder);
    }
    assert(RemoveDirectoryW(root));
    puts("Real Windows executable: Unicode CLI paths, load/edit/:w/:q/:wq, close protection, "
         "UTF-16 text and CRLF passed");
}
