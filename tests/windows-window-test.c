#include "../src/platform/windows/window.c"
#include <assert.h>
#include <wchar.h>

static void text(HWND window,const wchar_t *string)
{
    while(*string && IsWindow(window)) SendMessageW(window,WM_CHAR,*string++,1);
}

static void snapshot(struct windows_window *window)
{
    RECT rect; assert(GetClientRect(window->handle,&rect));
    HDC dc=GetDC(window->handle), memory=CreateCompatibleDC(dc); assert(dc && memory);
    BITMAPINFO info={0}; info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=rect.right; info.bmiHeader.biHeight=-rect.bottom;
    info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32; info.bmiHeader.biCompression=BI_RGB;
    void *pixels=NULL; HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&pixels,NULL,0);
    assert(bitmap && pixels); HGDIOBJ old=SelectObject(memory,bitmap);
    assert(paint_window(window,memory));
    const uint32_t *p=pixels;
    bool ink=false;
    for(int y=0;y<rect.bottom/2;++y) for(int x=0;x<rect.right;++x)
        if((p[y*rect.right+x]&0xffffff)!=0x18202b) ink=true;
    assert(ink);
    FILE *file=fopen("windows-editor.ppm","wb");assert(file);
    fprintf(file,"P6\n%d %d\n255\n",(int)rect.right,(int)rect.bottom);
    for(int i=0;i<rect.right*rect.bottom;++i){unsigned char rgb[]={p[i]>>16,p[i]>>8,p[i]};fwrite(rgb,1,3,file);}
    fclose(file); SelectObject(memory,old); DeleteObject(bitmap);DeleteDC(memory);ReleaseDC(window->handle,dc);
}

int main(void)
{
    struct app app;assert(app_open(&app,NULL));
    struct windows_window window;assert(open_window(&window,&app,SW_SHOWNORMAL));
    HWND handle=window.handle;
    text(handle,L"ihello\rworld");
    assert(!strcmp(app.editor.text,"hello\nworld"));
    text(handle,L"\x1b");text(handle,L"khjl");assert(app.editor.cursor==10);
    text(handle,L"i!");SendMessageW(handle,WM_KEYDOWN,VK_LEFT,1);
    SendMessageW(handle,WM_KEYDOWN,VK_DELETE,1);assert(!strcmp(app.editor.text,"hello\nworld"));
    text(handle,L"\r\t");text(handle,L"\b\b\b\b\b");assert(!strcmp(app.editor.text,"hello\nworld"));
    SendMessageW(handle,WM_CHAR,L'x',3);assert(strstr(app.editor.text,"xxx"));
    SendMessageW(handle,WM_CHAR,0xd83d,1);SendMessageW(handle,WM_CHAR,0xdcc4,1);
    assert(strstr(app.editor.text,"\xf0\x9f\x93\x84"));
    SendMessageW(handle,WM_CHAR,0xd800,1);SendMessageW(handle,WM_KILLFOCUS,0,0);
    text(handle,L"Z");assert(strstr(app.editor.text,"Z"));
    text(handle,L"\x1b"); SendMessageW(handle,WM_CLOSE,0,0);
    assert(IsWindow(handle) && app.running && strstr(app.editor.message,"Unsaved"));

    const UINT dpis[]={96,120,144,192};
    for(unsigned i=0;i<sizeof(dpis)/sizeof(dpis[0]);++i){
        RECT rect={40,40,840,600};
        SendMessageW(handle,WM_DPICHANGED,MAKELONG(dpis[i],dpis[i]),(LPARAM)&rect);
        assert(app.scale120==(uint32_t)MulDiv((int)dpis[i],120,96));
        InvalidateRect(handle,NULL,FALSE);UpdateWindow(handle);assert(!app.failed && !app.dirty);
    }
    text(handle,L":");text(handle,L"wq C:\\does-not-exist\\notes.txt\r");
    assert(IsWindow(handle) && app.running && app.editor.modified && strstr(app.editor.message,"Write:"));
    snapshot(&window);
    text(handle,L":q!\r");assert(!IsWindow(handle) && !app.running);
    app_close(&app); UnregisterClassW(window_class,GetModuleHandleW(NULL));
    puts("Win32 messages, input/repeat/UTF-16, close protection, DPI rendering and save/quit failures passed");
}
