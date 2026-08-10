#pragma once
// 游戏启动与进程监控：启动 java 进程，监控其退出以启停计时
#include <string>
#include <functional>
#include <windows.h>

class GameLauncher {
public:
    // 注册「进程退出」回调（用于在 UI 上停止计时）
    void SetOnExit(std::function<void()> cb) { onExit_ = cb; }

    // 启动游戏。javaExe: java.exe 路径；coreJar: 核心 jar 路径；memMb: 分配内存
    // 返回 true 表示成功启动
    bool Start(const std::wstring& javaExe, const std::wstring& coreJar, int memMb);

    bool IsRunning() const { return running_; }
    void Stop();

private:
    static DWORD WINAPI MonitorThread(LPVOID param);
    void MonitorLoop();

    HANDLE process_ = nullptr;
    bool running_ = false;
    std::function<void()> onExit_;
};
