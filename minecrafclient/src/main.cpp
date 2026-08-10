// 程序入口
#include "app.h"
#include <windows.h>

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    // WebView2 需要 COM 单元线程
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    App app;
    if (!app.Init(hInstance, nCmdShow)) {
        MessageBoxW(nullptr, L"启动器初始化失败，请检查 WebView2 运行时是否已安装。",
                    L"寄寄之家启动器", MB_ICONERROR);
        CoUninitialize();
        return 1;
    }

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    CoUninitialize();
    return 0;
}
