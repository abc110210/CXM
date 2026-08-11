#pragma once
// 主应用：创建窗口、协调 WebView2 / OAuth / 游戏启动 / 云端同步
#include <windows.h>
#include <string>
#include <thread>
#include <atomic>
#include "config.h"
#include "webview2_host.h"
#include "game_launcher.h"
#include "cloud_sync.h"

class App {
public:
    bool Init(HINSTANCE hInstance, int nCmdShow);
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

private:
    void OnWebMessage(const std::wstring& json);   // 处理 JS 发来的消息
    void OnNavigate(const std::wstring& uri);       // 拦截 stardust:// 回调
    void DoOAuthLogin();                            // 打开 LittleSkin 授权页
    void ParseCallback(const std::wstring& uri);    // 解析 code 并换 token
    void OnJsLaunchGame();                          // 启动游戏
    void OnGameExit();                              // 游戏进程退出
    void GameTickLoop();                           // 每秒向 JS 推送时长
    void SendToJs(const std::wstring& json);        // 给 JS 发消息
    void SelectJavaFolder();                        // 弹文件夹选择框选 Java bin
    void SendJavaList();                            // 回传 Java 列表给前端
    void SendCoreList();                            // 回传核心列表给前端

    HWND hwnd_ = nullptr;
    HINSTANCE hinst_ = nullptr;
    int nCmdShow_ = 0;

    Config cfg_;
    WebView2Host webview_;
    GameLauncher game_;
    CloudSync* cloud_ = nullptr;

    std::atomic<bool> playing_{false};
    std::atomic<long long> gameStartMs_{0};
    std::thread tickThread_;
    std::wstring currentPlayer_;
    bool loggedIn_ = false;
    bool pendingLoginSuccess_ = false;  // 导航回首页后触发登录态
    std::wstring pendingUsername_;
};
