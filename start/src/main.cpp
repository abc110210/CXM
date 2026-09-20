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
    EnsureDir(cfg.stateDir);
    EnsureDir(cfg.reportDir);

    g_hSvcStop = CreateEventW(NULL, TRUE, FALSE, NULL);
    HANDLE hGlobal = CreateGlobalStopEvent();
    HANDLE hLocal  = CreateEventW(NULL, TRUE, FALSE, STOP_EVENT_LOCAL);
    if (hGlobal) ResetEvent(hGlobal);
    if (hLocal)  ResetEvent(hLocal);

    AppendLog(cfg.stateDir, L"[service] DiskStress service starting");
    ReportSvcStatus(SERVICE_RUNNING, NO_ERROR, 0);

    RunWorker(cfg, hGlobal, hLocal, g_hSvcStop);

    AppendLog(cfg.stateDir, L"[service] DiskStress service stopped");
    if (hGlobal) CloseHandle(hGlobal);
    if (hLocal)  CloseHandle(hLocal);
    if (g_hSvcStop) CloseHandle(g_hSvcStop);
    ReportSvcStatus(SERVICE_STOPPED, NO_ERROR, 0);
}

// ------------------------------------------------------- install / remove ---

static bool InstallService(std::wstring& msg) {
    wchar_t path[MAX_PATH + 1] = {0};
    GetModuleFileNameW(NULL, path, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(path) + L"\" --service";

    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
    if (!scm) { msg = L"OpenSCManager failed, error=" + FormatInt(GetLastError()); return false; }

    SC_HANDLE exist = OpenServiceW(scm, SERVICE_NAME, SERVICE_QUERY_STATUS);
    if (exist) {
        CloseServiceHandle(exist);
        CloseServiceHandle(scm);
        msg = L"service already installed";
        return true;
    }

    SC_HANDLE svc = CreateServiceW(scm, SERVICE_NAME, SERVICE_DISPLAY,
                                   SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
                                   SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                                   cmd.c_str(), NULL, NULL, NULL, NULL, NULL);
    if (!svc) {
        msg = L"CreateService failed (needs administrator), error=" + FormatInt(GetLastError());
        CloseServiceHandle(scm);
        return false;
    }

    SERVICE_DESCRIPTION desc;
    desc.lpDescription = (LPWSTR)L"4 KiB 随机写入 + 每次 fsync 的磁盘压力测试，开机自动运行。";
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &desc);

    // restart the stress run if it ever dies on its own
    SC_ACTION actions[3];
    actions[0].Type  = SC_ACTION_RESTART;  actions[0].Delay = 10000;
    actions[1].Type  = SC_ACTION_RESTART;  actions[1].Delay = 30000;
    actions[2].Type  = SC_ACTION_NONE;     actions[2].Delay = 0;
    SERVICE_FAILURE_ACTIONSW fa;
    ZeroMemory(&fa, sizeof(fa));
    fa.dwResetPeriod = 86400;
    fa.cActions      = 3;
    fa.lpsaActions   = actions;
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);

    BOOL started = StartServiceW(svc, 0, NULL);
    msg = started ? L"service installed and started"
                  : L"service installed, StartService failed, error=" + FormatInt(GetLastError());

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
    EnsureDir(cfg.stateDir);

    std::wstring arg;
    if (__argc > 1) arg = __wargv[1];

    if (arg == L"--install" || arg == L"-i") {
        std::wstring msg;
        InstallService(msg);
        AppendLog(cfg.stateDir, L"[install] " + msg);
        return 0;
    }
    if (arg == L"--uninstall" || arg == L"-u") {
        std::wstring msg;
        RemoveService(msg);
        AppendLog(cfg.stateDir, L"[uninstall] " + msg);
        return 0;
    }

    if (arg == L"--service") {
        SERVICE_TABLE_ENTRYW table[2];
        table[0].lpServiceName = (LPWSTR)SERVICE_NAME;
        table[0].lpServiceProc = ServiceMain;
        table[1].lpServiceName = NULL;
        table[1].lpServiceProc = NULL;
        if (!StartServiceCtrlDispatcherW(table)) {
            AppendLog(cfg.stateDir, L"[service] StartServiceCtrlDispatcher failed, error=" +
                      FormatInt(GetLastError()));
            return 1;
        }
        return 0;
    }

    // --- interactive silent run: single instance + stop events ---
    HANDLE hMutex = CreateMutexW(NULL, TRUE, MUTEX_NAME);
    if (!hMutex) return 2;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        AppendLog(cfg.stateDir, L"[run] another instance is already running, exit.");
        return 0;
    }

    HANDLE hGlobal = CreateGlobalStopEvent();
    HANDLE hLocal  = CreateEventW(NULL, TRUE, FALSE, STOP_EVENT_LOCAL);
    if (hGlobal) ResetEvent(hGlobal);
    if (hLocal)  ResetEvent(hLocal);

    EnsureDir(cfg.reportDir);
    AppendLog(cfg.stateDir, L"[run] DiskStress started (interactive silent mode)");
    int rc = RunWorker(cfg, hGlobal, hLocal, NULL);
    AppendLog(cfg.stateDir, L"[run] DiskStress stopped");

    if (hGlobal) CloseHandle(hGlobal);
    if (hLocal)  CloseHandle(hLocal);
    CloseHandle(hMutex);
    return rc;
}
