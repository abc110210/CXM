// DiskStressStart.exe
//   no arguments      : silent background run (double click)
//   --install         : register + start the auto-start Windows service
//   --uninstall       : stop + remove the service
//   --service         : service entry (called by the SCM, not by hand)

#include "shared.h"
#include "worker.h"

#include <cstdio>
#include <cstdlib>
#include <sddl.h>

#pragma comment(lib, "advapi32.lib")

static SERVICE_STATUS        g_svcStatus;
static SERVICE_STATUS_HANDLE g_svcHandle = NULL;
static HANDLE                g_hSvcStop  = NULL;

// ----------------------------------------------------------------- service ---

static void ReportSvcStatus(DWORD state, DWORD exitCode, DWORD waitHint) {
    static DWORD checkpoint = 1;
    g_svcStatus.dwServiceType             = SERVICE_WIN32_OWN_PROCESS;
    g_svcStatus.dwCurrentState            = state;
    g_svcStatus.dwWin32ExitCode           = (state == SERVICE_STOPPED) ? exitCode : NO_ERROR;
    g_svcStatus.dwWaitHint                = waitHint;
    g_svcStatus.dwControlsAccepted        = (state == SERVICE_RUNNING)
                                              ? (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN) : 0;
    if (state == SERVICE_RUNNING || state == SERVICE_STOPPED) {
        g_svcStatus.dwCheckPoint = 0;
    } else {
        g_svcStatus.dwCheckPoint = checkpoint++;
    }
    if (g_svcHandle) SetServiceStatus(g_svcHandle, &g_svcStatus);
}

static void WINAPI ServiceCtrlHandler(DWORD ctrl) {
    if (ctrl == SERVICE_CONTROL_STOP || ctrl == SERVICE_CONTROL_SHUTDOWN) {
        ReportSvcStatus(SERVICE_STOP_PENDING, NO_ERROR, 180000);
        if (g_hSvcStop) SetEvent(g_hSvcStop);
        return;
    }
    ReportSvcStatus(g_svcStatus.dwCurrentState, NO_ERROR, 0);
}

// Everyone may query + signal the stop event, so a non-elevated console can stop us.
static HANDLE CreateGlobalStopEvent() {
    SECURITY_ATTRIBUTES sa;
    ZeroMemory(&sa, sizeof(sa));
    sa.nLength = sizeof(sa);

    PSECURITY_DESCRIPTOR sd = NULL;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(A;;0x001F0003;;;SY)(A;;0x001F0003;;;BA)(A;;0x00120003;;;WD)",
            SDDL_REVISION_1, &sd, NULL)) {
        sa.lpSecurityDescriptor = sd;
    }
    HANDLE h = CreateEventW(&sa, TRUE, FALSE, STOP_EVENT_GLOBAL);
    if (sd) LocalFree(sd);
    return h;   // NULL when the process cannot create global objects
}

static void WINAPI ServiceMain(DWORD, LPWSTR*) {
    g_svcHandle = RegisterServiceCtrlHandlerW(SERVICE_NAME, ServiceCtrlHandler);
    if (!g_svcHandle) return;

    ZeroMemory(&g_svcStatus, sizeof(g_svcStatus));
    ReportSvcStatus(SERVICE_START_PENDING, NO_ERROR, 10000);

    Config cfg = DefaultConfig();
    ResolveWritableDirs(cfg, GetExeDir());
    AppendDebugLog(cfg.stateDir, L"[OK]   服务入口 | SCM 已调用 ServiceMain，正在注册控制处理器");

    // 防止多开：服务模式同样检查全局单实例
    HANDLE hSingle = NULL;
    if (!AcquireSingleInstance(hSingle)) {
        AppendLog(cfg.stateDir, L"[service] another instance is already running, abort start");
        AppendDebugLog(cfg.stateDir, L"[FAIL] 单实例检查 | 已有实例在运行，服务启动中止");
        ReportSvcStatus(SERVICE_STOPPED, ERROR_SERVICE_ALREADY_RUNNING, 0);
        return;
    }
    AppendDebugLog(cfg.stateDir, L"[OK]   单实例检查 | 全局互斥体已持有，无其他实例");

    g_hSvcStop = CreateEventW(NULL, TRUE, FALSE, NULL);
    HANDLE hGlobal = CreateGlobalStopEvent();
    HANDLE hLocal  = CreateEventW(NULL, TRUE, FALSE, STOP_EVENT_LOCAL);
    if (hGlobal) ResetEvent(hGlobal);
    if (hLocal)  ResetEvent(hLocal);
    AppendDebugLog(cfg.stateDir, L"[OK]   停止事件 | Global=" +
                   std::wstring(hGlobal ? L"已创建" : L"不可用") +
                   L"，会话内=" + std::wstring(hLocal ? L"已创建" : L"不可用"));

    AppendLog(cfg.stateDir, L"[service] DiskStress service starting");
    ReportSvcStatus(SERVICE_RUNNING, NO_ERROR, 0);
    AppendDebugLog(cfg.stateDir, L"[OK]   服务状态 | 已上报 SERVICE_RUNNING");

    SpawnWatchdogProcess(cfg.stateDir);   // watchdog watches the service from now on
    int rc = RunWorker(cfg, hGlobal, hLocal, g_hSvcStop);
    AppendDebugLog(cfg.stateDir, L"[OK]   写入循环结束 | 返回码=" + FormatInt((uint64_t)rc));

    AppendLog(cfg.stateDir, L"[service] DiskStress service stopped");
    if (hGlobal) CloseHandle(hGlobal);
    if (hLocal)  CloseHandle(hLocal);
    if (g_hSvcStop) CloseHandle(g_hSvcStop);
    if (hSingle)  CloseHandle(hSingle);
    ReportSvcStatus(SERVICE_STOPPED, NO_ERROR, 0);
    AppendDebugLog(cfg.stateDir, L"[OK]   服务状态 | 已上报 SERVICE_STOPPED，进程即将退出");
}

// ------------------------------------------------------- install / remove ---

static bool InstallService(std::wstring& msg) {
    wchar_t path[MAX_PATH + 1] = {0};
    GetModuleFileNameW(NULL, path, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(path) + L"\" --service";

    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
    if (!scm) { msg = L"OpenSCManager failed, error=" + FormatInt(GetLastError()); return false; }

    // upsert: create it, or re-point an existing one at the current exe path
    SC_HANDLE svc = OpenServiceW(scm, SERVICE_NAME, SERVICE_ALL_ACCESS);
    bool updated = (svc != NULL);

    if (!svc) {
        svc = CreateServiceW(scm, SERVICE_NAME, SERVICE_DISPLAY,
                             SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
                             SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                             cmd.c_str(), NULL, NULL, NULL, NULL, NULL);
        if (!svc) {
            msg = L"CreateService failed (needs administrator), error=" + FormatInt(GetLastError());
            CloseServiceHandle(scm);
            return false;
        }
    } else {
        // stop the running instance before switching the binary path
        SERVICE_STATUS st;
        ZeroMemory(&st, sizeof(st));
        ControlService(svc, SERVICE_CONTROL_STOP, &st);
        for (int i = 0; i < 60; i++) {
            ZeroMemory(&st, sizeof(st));
            if (!QueryServiceStatus(svc, &st)) break;
            if (st.dwCurrentState == SERVICE_STOPPED) break;
            Sleep(1000);
        }
        if (!ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, SERVICE_AUTO_START, SERVICE_NO_CHANGE,
                                  cmd.c_str(), NULL, NULL, NULL, NULL, NULL, NULL)) {
            msg = L"ChangeServiceConfig failed, error=" + FormatInt(GetLastError());
            CloseServiceHandle(svc);
            CloseServiceHandle(scm);
            return false;
        }
    }

    SERVICE_DESCRIPTION desc;
    desc.lpDescription = (LPWSTR)L"4 KiB 随机写入 + 每次 fsync 的磁盘压力测试，开机自动运行。";
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &desc);

    // delayed auto start: boots a bit later, after the busiest boot I/O is over
    SERVICE_DELAYED_AUTO_START_INFO dai;
    ZeroMemory(&dai, sizeof(dai));
    dai.fDelayedAutostart = TRUE;
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &dai);

    // taskkill /f 等异常退出 = 服务失败 -> SCM 按此链无限重启；正常停止不算失败、不会复活
    SC_ACTION actions[3];
    actions[0].Type  = SC_ACTION_RESTART;  actions[0].Delay = 5000;   // 第一次失败 5s 后拉起
    actions[1].Type  = SC_ACTION_RESTART;  actions[1].Delay = 10000;
    actions[2].Type  = SC_ACTION_RESTART;  actions[2].Delay = 30000;  // SCM 之后永远重复此级 = 无限复活
    SERVICE_FAILURE_ACTIONSW fa;
    ZeroMemory(&fa, sizeof(fa));
    fa.dwResetPeriod = 0;      // 失败计数永不重置，重启链长期有效
    fa.cActions      = 3;
    fa.lpsaActions   = actions;
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);

    BOOL started = StartServiceW(svc, 0, NULL);
    std::wstring tail;
    if (started) tail = L" and started";
    else tail = L", StartService failed, error=" + FormatInt(GetLastError());
    msg = std::wstring(updated ? L"service updated to " : L"service installed at ")
          + std::wstring(path) + tail;

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return true;
}

static bool RemoveService(std::wstring& msg) {
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) { msg = L"OpenSCManager failed, error=" + FormatInt(GetLastError()); return false; }

    SC_HANDLE svc = OpenServiceW(scm, SERVICE_NAME,
                                 SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
    if (!svc) {
        CloseServiceHandle(scm);
        msg = L"service not installed";
        return true;
    }

    SERVICE_STATUS st;
    ZeroMemory(&st, sizeof(st));
    ControlService(svc, SERVICE_CONTROL_STOP, &st);
    for (int i = 0; i < 60; i++) {
        Sleep(1000);
        ZeroMemory(&st, sizeof(st));
        if (!QueryServiceStatus(svc, &st)) break;
        if (st.dwCurrentState == SERVICE_STOPPED) break;
    }

    bool ok = DeleteService(svc) == TRUE;
    msg = ok ? L"service stopped and removed"
             : L"DeleteService failed, error=" + FormatInt(GetLastError());
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok;
}

// ------------------------------------------------------------------ main ---

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    Config cfg = DefaultConfig();
    ResolveWritableDirs(cfg, GetExeDir());

    std::wstring arg;
    if (__argc > 1) arg = __wargv[1];

    AppendDebugLog(cfg.stateDir, L"[OK]   进程启动 | 参数=[" + arg + L"]，PID=" +
                   FormatInt(GetCurrentProcessId()));
    AppendDebugLog(cfg.stateDir, L"[OK]   配置加载 | 压力文件=" + cfg.filePath +
                   L"，覆盖区=" + FormatBytes(cfg.workingSetBytes) +
                   L"，报告目录=" + cfg.reportDir);

    if (arg == L"--install" || arg == L"-i") {
        std::wstring msg;
        bool ok = InstallService(msg);
        AppendLog(cfg.stateDir, L"[install] " + msg);
        AppendDebugLog(cfg.stateDir, (ok ? L"[OK]   安装服务 | " : L"[FAIL] 安装服务 | ") + msg);
        return 0;
    }
    if (arg == L"--uninstall" || arg == L"-u") {
        std::wstring msg;
        bool ok = RemoveService(msg);
        AppendLog(cfg.stateDir, L"[uninstall] " + msg);
        AppendDebugLog(cfg.stateDir, (ok ? L"[OK]   卸载服务 | " : L"[FAIL] 卸载服务 | ") + msg);
        return 0;
    }

    if (arg == L"--watchdog") {
        // Watchdog mode: single instance; every 60 s check the service,
        // start it when it is not RUNNING. Exits on the global stop event.
        HANDLE m = CreateMutexW(NULL, TRUE, WATCHDOG_MUTEX);
        if (!m) return 2;
        if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;   // another watchdog alive
        HANDLE hStop = CreateEventW(NULL, TRUE, FALSE, WATCHDOG_STOPEV);
        ResetEvent(hStop);
        AppendDebugLog(cfg.stateDir, L"[OK]   看门狗 | 启动 PID=" + FormatInt(GetCurrentProcessId()));
        SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
        while (hStop && WaitForSingleObject(hStop, 60000) == WAIT_TIMEOUT) {
            if (!scm) continue;
            SC_HANDLE svc = OpenServiceW(scm, SERVICE_NAME, SERVICE_QUERY_STATUS | SERVICE_START);
            if (!svc) continue;
            SERVICE_STATUS st;
            if (QueryServiceStatus(svc, &st) &&
                st.dwCurrentState != SERVICE_RUNNING &&
                st.dwCurrentState != SERVICE_START_PENDING &&
                st.dwCurrentState != SERVICE_STOP_PENDING) {
                if (StartServiceW(svc, 0, NULL))
                    AppendDebugLog(cfg.stateDir, L"[OK]   看门狗 | 服务未运行，已拉起");
                else
                    AppendDebugLog(cfg.stateDir, L"[FAIL] 看门狗 | StartService 失败 err=" +
                                   FormatInt(GetLastError()));
            }
            CloseServiceHandle(svc);
        }
        AppendDebugLog(cfg.stateDir, L"[OK]   看门狗 | 收到停止信号，退出");
        if (hStop) CloseHandle(hStop);
        if (m)     CloseHandle(m);
        if (scm)   CloseServiceHandle(scm);
        return 0;
    }
    if (arg == L"--stopwatchdog") {
        // Used by the stop flow: watchdog must die before the service stops.
        SignalWatchdogStop(cfg.stateDir);
        return 0;
    }

    if (arg == L"--service") {
        SERVICE_TABLE_ENTRYW table[2];
        table[0].lpServiceName = (LPWSTR)SERVICE_NAME;
        table[0].lpServiceProc = ServiceMain;
        table[1].lpServiceName = NULL;
        table[1].lpServiceProc = NULL;
        AppendDebugLog(cfg.stateDir, L"[OK]   服务模式 | 正在连接 SCM 控制分发器");
        if (!StartServiceCtrlDispatcherW(table)) {
            AppendLog(cfg.stateDir, L"[service] StartServiceCtrlDispatcher failed, error=" +
                      FormatInt(GetLastError()));
            AppendDebugLog(cfg.stateDir, L"[FAIL] 服务模式 | StartServiceCtrlDispatcher 失败 err=" +
                           FormatInt(GetLastError()) + L"（必须由 SCM 启动，勿手动加 --service）");
            return 1;
        }
        return 0;
    }

    // --- interactive silent run: single instance + stop events ---
    AppendDebugLog(cfg.stateDir, L"[OK]   交互模式 | 双击启动，无参数，PID=" +
                   FormatInt(GetCurrentProcessId()));

    HANDLE hMutex = NULL;
    if (!AcquireSingleInstance(hMutex)) {
        AppendLog(cfg.stateDir, L"[run] another instance is already running, exit.");
        AppendDebugLog(cfg.stateDir, L"[FAIL] 单实例检查 | 已有实例在运行（含服务模式），本次直接退出");
        return 0;
    }
    AppendDebugLog(cfg.stateDir, L"[OK]   单实例检查 | 全局互斥体已持有，无其他实例");

    HANDLE hGlobal = CreateGlobalStopEvent();
    HANDLE hLocal  = CreateEventW(NULL, TRUE, FALSE, STOP_EVENT_LOCAL);
    if (hGlobal) ResetEvent(hGlobal);
    if (hLocal)  ResetEvent(hLocal);
    AppendDebugLog(cfg.stateDir, L"[OK]   停止事件 | Global=" +
                   std::wstring(hGlobal ? L"已创建" : L"不可用") +
                   L"，会话内=" + std::wstring(hLocal ? L"已创建" : L"不可用"));

    EnsureDir(cfg.reportDir);
    AppendLog(cfg.stateDir, L"[run] DiskStress started (interactive silent mode)");
    AppendDebugLog(cfg.stateDir, L"[OK]   日志目录 | 状态=" + cfg.stateDir +
                   L"，报告=" + cfg.reportDir);
    SpawnWatchdogProcess(cfg.stateDir);   // interactive mode: watchdog too
    int rc = RunWorker(cfg, hGlobal, hLocal, NULL);
    AppendDebugLog(cfg.stateDir, L"[OK]   写入循环结束 | 返回码=" + FormatInt((uint64_t)rc));
    AppendLog(cfg.stateDir, L"[run] DiskStress stopped");

    if (hGlobal) CloseHandle(hGlobal);
    if (hLocal)  CloseHandle(hLocal);
    if (hMutex)  CloseHandle(hMutex);
    return rc;
}
