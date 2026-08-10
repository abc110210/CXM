// 游戏启动实现
#include "game_launcher.h"
#include "common.h"

bool GameLauncher::Start(const std::wstring& javaExe, const std::wstring& coreJar, int memMb) {
    if (running_) return false;

    // 构造启动命令。真实 Minecraft 还需要 classpath/natives/version 等参数，
    // 这里给出最小可运行框架，后续按官方启动协议补全。
    std::wstring cmd = L"\"" + javaExe + L"\""
                       + L" -Xmx" + std::to_wstring(memMb) + L"M"
                       + L" -Xms" + std::to_wstring(memMb) + L"M"
                       + L" -jar \"" + coreJar + L"\"";

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    // CreateProcess 会修改命令行缓冲区，必须可写
    std::wstring cmdMutable = cmd;
    if (!CreateProcessW(nullptr, &cmdMutable[0], nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        util::Log("启动游戏失败，错误码: " + std::to_string(GetLastError()));
        return false;
    }
    process_ = pi.hProcess;
    CloseHandle(pi.hThread);
    running_ = true;

    // 起监控线程
    CreateThread(nullptr, 0, MonitorThread, this, 0, nullptr);
    util::Log("游戏进程已启动");
    return true;
}

DWORD WINAPI GameLauncher::MonitorThread(LPVOID param) {
    ((GameLauncher*)param)->MonitorLoop();
    return 0;
}

void GameLauncher::MonitorLoop() {
    if (process_) {
        WaitForSingleObject(process_, INFINITE);
        CloseHandle(process_);
        process_ = nullptr;
    }
    running_ = false;
    util::Log("游戏进程已退出");
    if (onExit_) onExit_();
}

void GameLauncher::Stop() {
    if (process_) {
        TerminateProcess(process_, 0);
        CloseHandle(process_);
        process_ = nullptr;
    }
    running_ = false;
}
