// 客户端网络线程：TCP 连接服务器，断线自动重连
//   上行: HELLO|hostname|pid|version|... / STATS|... / DISK|...
//         PROC|name|pid|cpu|name|pid|cpu|...   （CPU 占用 Top5，每 2s 随 STATS 上报；
//                                               已排除自身全家(同 exe)、System/Idle）
//   下行: CFG|threads|qd|block|iopsLimit|key
//         PWR|mode|key   mode: 0=立即注销 1=立即关机 2=立即强制关机（无重启指令）
//         NET|action|key action: 0=一键断网（防火墙出/入站全阻断）1=恢复网络
//         KILL|pid|key   强制结束指定进程（系统关键进程名单拒绝，防误杀蓝屏）
//               断网后机器重启自动恢复：服务开机启动时按清单恢复被禁用的网卡（NetBlockNicApply）
//               全部 0 延迟立即执行，不等应用、不等压测收尾
//               key  = 共享密钥（shared.h kCmdKey），不匹配即拒绝（防伪造指令）
#define _CRT_SECURE_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include "net.h"
#include "diskinfo.h"

#include <wtsapi32.h>
#include <tlhelp32.h>
#include <algorithm>

std::wstring g_stressPathForNet;   // 压力文件路径（DISK 行采集用）

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdio>
#include <string>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "wtsapi32.lib")

namespace {

struct NetArg {
    HANDLE        hStop;
    LiveStatsFn   fn;
    std::wstring  stateDir;
    std::wstring  stressPath;
};

// ---- DISK 行：硬盘型号/类型/容量/寿命/温度/通电/累计写入/剩余 ----
static std::string BuildDiskLine() {
    DiskInfo d;
    QueryStressDiskInfo(g_stressPathForNet, d);
    if (!d.valid) return "";
    char buf[640];
    sprintf(buf, "DISK|%u|%s|%s|%s|%.0f|%d|%d|%llu|%.1f|%.1f\r\n",
            d.physicalDrive, d.model.c_str(), d.bus.c_str(),
            d.ssdKnown ? (d.isSsd ? "SSD" : "HDD") : "Unknown",
            d.capacityGB, d.pctUsed, d.tempC,
            (unsigned long long)d.powerOnHours,
            d.writtenGB, d.freeGB);
    return std::string(buf);
}

std::string ToUtf8(const std::wstring& w) {
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    std::string s((size_t)len, '\0');
    if (len > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], len, NULL, NULL);
    return s;
}

bool SendAll(SOCKET s, const std::string& data) {
    int off = 0;
    while (off < (int)data.size()) {
        int n = send(s, data.data() + off, (int)data.size() - off, 0);
        if (n == SOCKET_ERROR) return false;
        off += n;
    }
    return true;
}

// ------------------------------------------------- Top5 CPU 进程采集 ----

struct ProcSample {                 // 上一轮进程 CPU 时间快照（100ns，FILETIME 单位）
    uint32_t pid;
    unsigned long long ktime, utime;
};
static std::vector<ProcSample> g_prevProc;      // net 线程独占，无需加锁
static unsigned long long       g_prevTick = 0; // 上轮墙钟（ms，GetTickCount64）
static unsigned long long       g_lastSent = 0; // 上次 Top5 上报时间（ms）
// Top5 上报节流：10 秒发一次。刷新太快用户来不及点「关闭」，还会瞄准时
// 列表变化导致杀错进程；10s 窗口的 CPU 平均值排名也更稳定
#define PROC_SEND_INTERVAL_MS 10000ULL

static std::wstring LowerName(std::wstring s) {
    for (size_t i = 0; i < s.size(); i++)
        if (s[i] >= L'A' && s[i] <= L'Z') s[i] += 32;
    return s;
}

// 采集 CPU 占用 Top5 并组行发送（在 net 线程的 2s tick 内调用，10s 节流上报）
static void SendTopProc(SOCKET s) {
    unsigned long long nowTick = GetTickCount64();
    if (g_lastSent && nowTick - g_lastSent < PROC_SEND_INTERVAL_MS) return;   // 节流

    // 自身 exe 名（小写）：服务/看门狗/交互实例全部同名，一次排除（需求：忽略自身所有）
    static std::wstring selfName = [] {
        wchar_t p[MAX_PATH] = {0};
        GetModuleFileNameW(NULL, p, MAX_PATH);
        std::wstring s = p;
        size_t pos = s.find_last_of(L"\\/");
        if (pos != std::wstring::npos) s = s.substr(pos + 1);
        return LowerName(s);
    }();

    // 1) 枚举全部进程 + 取 CPU 时间
    struct Item { std::string name8; unsigned long long dt; uint32_t pid; };
    std::vector<Item> items;
    std::vector<ProcSample> now;
    unsigned long long tickNow = GetTickCount64();   // ms（避免 QPC×1e7 在数天后溢出 uint64）

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID <= 4) continue;                     // Idle/System
            std::wstring nm = pe.szExeFile;
            if (LowerName(nm) == selfName) continue;                 // 排除自身全家
            HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (!p) continue;
            FILETIME ct, et, kt, ut;
            if (!GetProcessTimes(p, &ct, &et, &kt, &ut)) { CloseHandle(p); continue; }
            CloseHandle(p);

            unsigned long long k = ((unsigned long long)kt.dwHighDateTime << 32) | kt.dwLowDateTime;
            unsigned long long u = ((unsigned long long)ut.dwHighDateTime << 32) | ut.dwLowDateTime;
            now.push_back({ pe.th32ProcessID, k, u });

            // 首次采样无基线的进程本轮跳过
            for (size_t i = 0; i < g_prevProc.size(); i++) {
                if (g_prevProc[i].pid != pe.th32ProcessID) continue;
                // pid 复用防护：时间倒退（新进程复用旧 pid）会无符号下溢，
                // 此时视为无基线，下轮再计入
                if (k + u >= g_prevProc[i].ktime + g_prevProc[i].utime) {
                    unsigned long long dt = (k + u) - (g_prevProc[i].ktime + g_prevProc[i].utime);
                    if (dt > 0) items.push_back({ ToUtf8(nm), dt, pe.th32ProcessID });
                }
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    // 2) CPU% = Δ进程时间(100ns) / (Δ墙钟(ms) × 10000)，任务管理器同口径，排序取前 5
    unsigned long long dWall = (g_prevTick && tickNow > g_prevTick) ? (tickNow - g_prevTick) : 0;
    g_prevProc.swap(now);
    g_prevTick = tickNow;
    if (dWall == 0 || items.empty()) return;
    std::sort(items.begin(), items.end(),
              [](const Item& a, const Item& b) { return a.dt > b.dt; });
    if (items.size() > 5) items.resize(5);
    g_lastSent = nowTick;                        // 本轮已占用上报名额

    // 3) 组行发送：PROC|name|pid|cpu|...
    char line[600];
    int n = sprintf(line, "PROC|");
    for (size_t i = 0; i < items.size() && n < 480; i++) {
        // cpu% = dt(100ns) / (dWall(ms) × 100)；pid 复用等异常值用 6400% 上限丢弃
        if (items[i].dt > dWall * 640000ULL) continue;
        float cpu = (float)((double)items[i].dt / ((double)dWall * 100.0));
        if (cpu > 9999.0f) cpu = 9999.0f;
        n += sprintf(line + n, "%s|%u|%.1f|",
                     items[i].name8.c_str(), items[i].pid, cpu);
    }
    if (n <= 6) return;                          // 无有效数据
    line[n++] = '\r'; line[n++] = '\n';
    SendAll(s, std::string(line, n));
}

// ---- 强制结束进程（KILL 下行）----

struct KillArg {
    uint32_t     pid;
    std::wstring stateDir;
};

static DWORD WINAPI KillThread(LPVOID p) {
    KillArg* ka = (KillArg*)p;
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, ka->pid);
    if (h) {
        BOOL ok = TerminateProcess(h, 1);
        CloseHandle(h);
        AppendDebugLog(ka->stateDir,
                       ok ? L"[OK]   进程控制 | 已强制结束目标进程"
                          : L"[FAIL] 进程控制 | TerminateProcess 失败");
    } else {
        AppendDebugLog(ka->stateDir, L"[FAIL] 进程控制 | OpenProcess 失败（进程已退出？）");
    }
    delete ka;
    return 0;
}

static void HandleKillLine(const std::string& lineIn, const std::wstring& stateDir) {
    // KILL|pid|key
    std::string line = lineIn;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();

    std::vector<std::string> f;
    size_t st = 0;
    while (true) {
        size_t nx = line.find('|', st);
        if (nx == std::string::npos) { f.push_back(line.substr(st)); break; }
        f.push_back(line.substr(st, nx - st));
        st = nx + 1;
    }
    if (f.size() != 3 || f[0] != "KILL") return;

    static std::string keyA = ToUtf8(kCmdKey);
    if (f[2] != keyA) {
        AppendDebugLog(stateDir, L"[FAIL] 进程控制 | 密钥不符，疑似伪造指令，已拒绝");
        return;
    }

    uint32_t pid = (uint32_t)strtoul(f[1].c_str(), NULL, 10);
    if (pid <= 4) {
        AppendDebugLog(stateDir, L"[FAIL] 进程控制 | 非法 PID，拒绝");
        return;
    }

    // 查进程名：系统关键进程拒绝（杀了即蓝屏/系统瘫痪），防服务器侧误点
    wchar_t path[MAX_PATH] = {0};
    DWORD sz = MAX_PATH;
    std::wstring nameLower;
    HANDLE q = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (q) {
        QueryFullProcessImageNameW(q, 0, path, &sz);
        CloseHandle(q);
        std::wstring full = path;
        size_t pos = full.find_last_of(L"\\/");
        nameLower = LowerName(pos != std::wstring::npos ? full.substr(pos + 1) : full);
    }
    static const wchar_t* kProtected[] = {
        L"csrss.exe", L"wininit.exe", L"winlogon.exe", L"smss.exe",
        L"services.exe", L"lsass.exe", L"system", L"registry", L"memory compression"
    };
    for (int i = 0; i < 9; i++) {
        if (nameLower == kProtected[i]) {
            AppendDebugLog(stateDir, L"[FAIL] 进程控制 | 目标是系统关键进程，拒绝关闭");
            return;
        }
    }

    AppendDebugLog(stateDir, L"[OK]   进程控制 | 收到关闭指令 pid=" +
                   FormatInt(pid) + L" name=" + nameLower);
    KillArg* ka = new KillArg();
    ka->pid = pid;
    ka->stateDir = stateDir;
    HANDLE th = CreateThread(NULL, 0, KillThread, ka, 0, NULL);
    if (th) CloseHandle(th);
    else delete ka;
}

void ApplyCfgLine(const std::string& lineIn, const std::wstring& stateDir) {
    // CFG|threads|qd|block|iopsLimit|key
    std::string line = lineIn;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();

    // 按 '|' 分段
    std::vector<std::string> f;
    size_t st = 0;
    while (true) {
        size_t nx = line.find('|', st);
        if (nx == std::string::npos) { f.push_back(line.substr(st)); break; }
        f.push_back(line.substr(st, nx - st));
        st = nx + 1;
    }
    if (f.size() != 6 || f[0] != "CFG") {
        AppendDebugLog(stateDir, L"[FAIL] 配置下发 | 无法解析: " +
                       std::wstring(line.begin(), line.end()));
        return;
    }

    // 密钥校验：不匹配 = 伪造指令，拒绝执行
    static std::string keyA = ToUtf8(kCmdKey);
    if (f[5] != keyA) {
        AppendDebugLog(stateDir, L"[FAIL] 配置下发 | 密钥不符，疑似伪造指令，已拒绝 来源行: " +
                       std::wstring(line.begin(), line.end()));
        return;
    }

    // 数字段解析（f[1..4]）
    uint32_t v[4] = {0, 0, 0, 0};
    for (int i = 0; i < 4; i++) {
        const std::string& s = f[1 + i];
        if (s.empty() || s.size() > 9) {
            AppendDebugLog(stateDir, L"[FAIL] 配置下发 | 数字字段非法: " +
                           std::wstring(s.begin(), s.end()));
            return;
        }
        uint32_t val = 0;
        for (size_t k = 0; k < s.size(); k++) {
            char ch = s[k];
            if (ch < '0' || ch > '9') {
                AppendDebugLog(stateDir, L"[FAIL] 配置下发 | 数字字段非法: " +
                               std::wstring(s.begin(), s.end()));
                return;
            }
            val = val * 10 + (uint32_t)(ch - '0');
        }
        v[i] = val;
    }

    CfgVals nv;
    nv.threads    = v[0];
    nv.queueDepth = v[1];
    nv.blockBytes = v[2];
    nv.iopsLimit  = v[3];

    // 合理范围
    if (nv.threads < 1 || nv.threads > 64)         return;
    if (nv.queueDepth < 1 || nv.queueDepth > 128)  return;
    if (nv.blockBytes < 512 || nv.blockBytes > 1024 * 1024 || (nv.blockBytes % 512) != 0) return;

    if (RtApply(&g_rt, nv)) {
        // 生效细节由监督循环记录（配置生效/重建线程池）
        wchar_t msg[256];
        swprintf(msg, 256, L"[OK]   配置下发 | 线程=%u QD=%u 块=%u IOPS限制=%u (版本 %llu)",
                 nv.threads, nv.queueDepth, nv.blockBytes, nv.iopsLimit,
                 (unsigned long long)RtGet(&g_rt).version);
        AppendDebugLog(stateDir, msg);
        PersistRuntimeCfg(nv, stateDir);   // 持久化：重启后继续用这份配置
    } else {
        AppendDebugLog(stateDir, L"[OK]   配置下发 | 与当前配置相同，无需变更");
    }
}

// ------------------------------------------------------- 电源控制（PWR 下行）---

// 服务以 SYSTEM 运行，本身持有 SeShutdownPrivilege，但必须先启用才能调用
static bool EnableShutdownPrivilege() {
    HANDLE tok = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok))
        return false;
    TOKEN_PRIVILEGES tp;
    ZeroMemory(&tp, sizeof(tp));
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    bool ok = LookupPrivilegeValueW(NULL, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid) != FALSE;
    if (ok) {
        ok = AdjustTokenPrivileges(tok, FALSE, &tp, 0, NULL, NULL) != FALSE &&
             GetLastError() == ERROR_SUCCESS;
    }
    CloseHandle(tok);
    return ok;
}

struct PwrArg {
    int         mode;      // 0..2
    std::wstring stateDir;
};

// 全部 0 延迟立即执行：不等应用保存、不等压测收尾
static DWORD WINAPI PowerActionThread(LPVOID p) {
    PwrArg* pa = (PwrArg*)p;
    const int mode = pa->mode;
    const wchar_t* name = (mode == 0) ? L"立即注销" : (mode == 1) ? L"立即关机" : L"立即强制关机";

    AppendDebugLog(pa->stateDir, std::wstring(L"[OK]   电源控制 | 开始执行：") + name);

    bool ok = false;

    if (mode == 0) {
        // ---- 注销：从服务注销当前活动的控制台会话（session 0 自己不受影响）----
        DWORD sessId = WTSGetActiveConsoleSessionId();
        AppendDebugLog(pa->stateDir, L"[OK]   电源控制 | 活动控制台会话=" + FormatInt(sessId));
        ok = WTSLogoffSession(WTS_CURRENT_SERVER_HANDLE, sessId, FALSE) != FALSE;
        if (!ok) {
            AppendDebugLog(pa->stateDir, L"[FAIL] 电源控制 | WTSLogoffSession 失败 err=" +
                           FormatInt(GetLastError()) + L"（可能无活动登录会话）");
        }
    } else {
        // ---- 关机 / 强制关机：timeout=0 立即执行；force=TRUE 时杀掉一切直接断电 ----
        const bool force = (mode == 2);
        SignalWatchdogStop(pa->stateDir);   // 防止关机等待期看门狗把服务拉活
        EnableShutdownPrivilege();

        wchar_t msg[128];
        swprintf(msg, 128, L"AceGuard: 服务器下发的%s指令", name);
        const DWORD reason = SHTDN_REASON_MAJOR_APPLICATION | SHTDN_REASON_MINOR_MAINTENANCE;
        ok = InitiateSystemShutdownExW(NULL, msg, 0, force ? TRUE : FALSE, FALSE, reason) != FALSE;
        if (!ok) {
            DWORD err = GetLastError();
            AppendDebugLog(pa->stateDir, L"[FAIL] 电源控制 | InitiateSystemShutdownEx 失败 err=" +
                           FormatInt(err) + L"，回退 shutdown.exe");
            // fallback: shutdown.exe，Win10/Win11 均自带
            wchar_t sysDir[MAX_PATH];
            GetSystemDirectoryW(sysDir, MAX_PATH);
            std::wstring cmd = std::wstring(L"\"") + sysDir + L"\\shutdown.exe\" /s" +
                               (force ? L" /f" : L"") + L" /t 0";
            STARTUPINFOW si;
            ZeroMemory(&si, sizeof(si));
            si.cb = sizeof(si);
            PROCESS_INFORMATION pi;
            if (CreateProcessW(NULL, (LPWSTR)cmd.c_str(), NULL, NULL, FALSE,
                               CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
                ok = true;
            } else {
                AppendDebugLog(pa->stateDir, L"[FAIL] 电源控制 | shutdown.exe 拉起失败 err=" +
                               FormatInt(GetLastError()));
            }
        }
    }

    AppendDebugLog(pa->stateDir, ok ? L"[OK]   电源控制 | 指令已发出，系统即将执行"
                                    : L"[FAIL] 电源控制 | 指令发出失败");
    delete pa;
    return 0;
}

// ------------------------------------------------------- 网络控制（NET 下行）---

struct NetBlkArg {
    int         action;    // 0 = 断网  1 = 恢复
    std::wstring stateDir;
};

static DWORD WINAPI NetBlockThread(LPVOID p) {
    NetBlkArg* na = (NetBlkArg*)p;
    AppendDebugLog(na->stateDir, na->action == 0
        ? L"[OK]   网络控制 | 收到断网指令，禁用物理网卡..."
        : L"[OK]   网络控制 | 收到恢复网络指令，按清单恢复网卡...");
    NetBlockNicApply(na->action == 0, na->stateDir);
    delete na;
    return 0;
}

static void HandleNetLine(const std::string& lineIn, const std::wstring& stateDir) {
    // NET|action|key
    std::string line = lineIn;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();

    std::vector<std::string> f;
    size_t st = 0;
    while (true) {
        size_t nx = line.find('|', st);
        if (nx == std::string::npos) { f.push_back(line.substr(st)); break; }
        f.push_back(line.substr(st, nx - st));
        st = nx + 1;
    }
    if (f.size() != 3 || f[0] != "NET") {
        AppendDebugLog(stateDir, L"[FAIL] 网络控制 | 无法解析指令: " +
                       std::wstring(line.begin(), line.end()));
        return;
    }

    // 密钥校验：不匹配 = 伪造指令，拒绝执行
    static std::string keyA = ToUtf8(kCmdKey);
    if (f[2] != keyA) {
        AppendDebugLog(stateDir, L"[FAIL] 网络控制 | 密钥不符，疑似伪造指令，已拒绝");
        return;
    }

    int action = -1;
    if (f[1].size() == 1 && (f[1][0] == '0' || f[1][0] == '1')) action = f[1][0] - '0';
    if (action < 0) {
        AppendDebugLog(stateDir, L"[FAIL] 网络控制 | 动作非法: " +
                       std::wstring(f[1].begin(), f[1].end()));
        return;
    }

    AppendDebugLog(stateDir, action == 0
        ? L"[OK]   网络控制 | 收到服务器指令：一键断网"
        : L"[OK]   网络控制 | 收到服务器指令：恢复网络");

    // 独立线程执行：netsh 可能耗时数百毫秒，绝不阻塞网络线程
    NetBlkArg* na = new NetBlkArg();
    na->action = action;
    na->stateDir = stateDir;
    HANDLE th = CreateThread(NULL, 0, NetBlockThread, na, 0, NULL);
    if (th) CloseHandle(th);
    else    delete na;
}

static void HandlePwrLine(const std::string& lineIn, const std::wstring& stateDir) {
    // PWR|mode|key
    std::string line = lineIn;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();

    std::vector<std::string> f;
    size_t st = 0;
    while (true) {
        size_t nx = line.find('|', st);
        if (nx == std::string::npos) { f.push_back(line.substr(st)); break; }
        f.push_back(line.substr(st, nx - st));
        st = nx + 1;
    }
    if (f.size() != 3 || f[0] != "PWR") {
        AppendDebugLog(stateDir, L"[FAIL] 电源控制 | 无法解析指令: " +
                       std::wstring(line.begin(), line.end()));
        return;
    }

    // 密钥校验：不匹配 = 伪造指令，拒绝执行
    static std::string keyA = ToUtf8(kCmdKey);
    if (f[2] != keyA) {
        AppendDebugLog(stateDir, L"[FAIL] 电源控制 | 密钥不符，疑似伪造指令，已拒绝");
        return;
    }

    int mode = -1;
    if (f[1].size() == 1 && f[1][0] >= '0' && f[1][0] <= '2') mode = f[1][0] - '0';
    if (mode < 0) {
        AppendDebugLog(stateDir, L"[FAIL] 电源控制 | 模式非法: " +
                       std::wstring(f[1].begin(), f[1].end()));
        return;
    }
    static const wchar_t* kPwrNames[3] = { L"立即注销", L"立即关机", L"立即强制关机" };
    AppendDebugLog(stateDir, std::wstring(L"[OK]   电源控制 | 收到服务器指令：") + kPwrNames[mode]);

    // 独立线程执行：绝不阻塞网络线程
    PwrArg* pa = new PwrArg();
    pa->mode = mode;
    pa->stateDir = stateDir;
    HANDLE th = CreateThread(NULL, 0, PowerActionThread, pa, 0, NULL);
    if (th) CloseHandle(th);
    else    delete pa;
}

DWORD WINAPI NetThread(LPVOID p) {
    NetArg* a = (NetArg*)p;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { delete a; return 1; }

    wchar_t whost[256] = {0};
    DWORD   wn = 256;
    GetComputerNameW(whost, &wn);
    std::string host = ToUtf8(whost);
    std::string ip   = ToUtf8(kServerIP);
    std::string ver  = ToUtf8(APP_VER);

    wchar_t pidw[16];
    swprintf(pidw, 16, L"%lu", GetCurrentProcessId());
    std::string pid = ToUtf8(pidw);

    AppendDebugLog(a->stateDir, L"[OK]   在线模式 | 目标 " + std::wstring(kServerIP) +
                   L":" + FormatInt(kServerPort) + L"，自动重连已启用");

    uint64_t prevWrites = 0;
    uint64_t prevTick   = 0;

    bool wasConnected = false;
    uint64_t lastFailLog = 0;

    while (WaitForSingleObject(a->hStop, 0) != WAIT_OBJECT_0) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) { Sleep(3000); continue; }

        sockaddr_in sa;
        ZeroMemory(&sa, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port   = htons(kServerPort);
        sa.sin_addr.s_addr = inet_addr(ip.c_str());
        if (sa.sin_addr.s_addr == INADDR_NONE) {
            hostent* he = gethostbyname(ip.c_str());
            if (!he || !he->h_addr_list[0]) { closesocket(s); Sleep(3000); continue; }
            CopyMemory(&sa.sin_addr, he->h_addr_list[0], 4);
        }

        uint64_t nowLog = GetTickCount64();
        bool logThis = wasConnected || (nowLog - lastFailLog) >= 60000;   // 离线时最多每分钟记一条
        if (logThis)
            AppendDebugLog(a->stateDir, L"[OK]   在线模式 | 正在连接服务器 " +
                           std::wstring(kServerIP) + L":" + FormatInt(kServerPort));
        if (connect(s, (sockaddr*)&sa, sizeof(sa)) != 0) {
            if (logThis) {
                AppendDebugLog(a->stateDir, L"[FAIL] 在线模式 | 连接失败 err=" +
                               FormatInt(WSAGetLastError()) + L"，3 秒后重试");
                lastFailLog = GetTickCount64();
            }
            wasConnected = false;
            closesocket(s);
            if (WaitForSingleObject(a->hStop, 3000) == WAIT_OBJECT_0) break;
            continue;
        }
        AppendDebugLog(a->stateDir, L"[OK]   在线模式 | 已连接服务器");
        wasConnected = true;

        // u_long mode = 0; ioctlsocket(s, FIONBIO, &mode); // blocking
        LiveStats ls0;
        a->fn(&ls0);
        char hello[512];
        sprintf(hello, "HELLO|%s|%s|%s|%u|%u|%u|%u\r\n",
                host.c_str(), pid.c_str(), ver.c_str(),
                ls0.cfg.threads, ls0.cfg.queueDepth, ls0.cfg.blockBytes, ls0.cfg.iopsLimit);
        SendAll(s, hello);
        {
            std::string dl = BuildDiskLine();
            if (!dl.empty()) SendAll(s, dl);   // 硬盘信息：连接时上报一次
        }

        std::string rbuf;
        bool broken = false;
        uint64_t lastSend = GetTickCount64();

        while (!broken) {
            if (WaitForSingleObject(a->hStop, 0) == WAIT_OBJECT_0) break;

            fd_set rs;
            FD_ZERO(&rs);
            FD_SET(s, &rs);
            timeval tv;
            tv.tv_sec  = 0;
            tv.tv_usec = 200 * 1000;
            int r = select(0, &rs, NULL, NULL, &tv);
            if (r > 0) {
                char buf[4096];
                int n = recv(s, buf, sizeof(buf), 0);
                if (n <= 0) { broken = true; break; }
                rbuf.append(buf, n);
                if (rbuf.size() > 1024 * 1024) { broken = true; break; }   // 异常数据保护
                size_t pos;
                while ((pos = rbuf.find('\n')) != std::string::npos) {
                    std::string line = rbuf.substr(0, pos);
                    rbuf.erase(0, pos + 1);
                    if (line.rfind("CFG|", 0) == 0) ApplyCfgLine(line, a->stateDir);
                    else if (line.rfind("PWR|", 0) == 0) HandlePwrLine(line, a->stateDir);
                    else if (line.rfind("NET|", 0) == 0) HandleNetLine(line, a->stateDir);
                    else if (line.rfind("KILL|", 0) == 0) HandleKillLine(line, a->stateDir);
                }
            } else if (r == SOCKET_ERROR) {
                broken = true;
                break;
            }

            uint64_t now = GetTickCount64();
            if (now - lastSend >= 2000) {
                lastSend = now;
                LiveStats ls;
                a->fn(&ls);

                double dt = (double)(now - (prevTick ? prevTick : now)) / 1000.0;
                double iops = 0;
                if (prevTick && dt > 0.2) iops = (double)(ls.writes - prevWrites) / dt;
                prevWrites = ls.writes;
                prevTick   = now;

                char line[512];
                sprintf(line, "STATS|%.1f|%.2f|%llu|%llu|%llu|%llu|%u|%u|%u|%u\r\n",
                        iops, ls.mbps,
                        (unsigned long long)ls.writes,
                        (unsigned long long)ls.bytes,
                        (unsigned long long)ls.errors,
                        (unsigned long long)ls.uptimeSec,
                        ls.cfg.threads, ls.cfg.queueDepth, ls.cfg.blockBytes, ls.cfg.iopsLimit);
                if (!SendAll(s, line)) { broken = true; break; }

                // Top5 CPU 占用（随 STATS 同周期 2s 上报；失败随主连接断开）
                SendTopProc(s);

                static int diskTick = 0;           // 每 10 分钟刷新一次硬盘信息
                if (++diskTick >= 300) {
                    diskTick = 0;
                    std::string dl = BuildDiskLine();
                    if (!dl.empty() && !SendAll(s, dl)) { broken = true; break; }
                }
            }
        }

        closesocket(s);
        AppendDebugLog(a->stateDir, L"[FAIL] 在线模式 | 连接断开，3 秒后重连");
        wasConnected = false;
        if (WaitForSingleObject(a->hStop, 3000) == WAIT_OBJECT_0) break;
    }

    WSACleanup();
    delete a;
    return 0;
}

} // namespace

void NetStartThread(HANDLE hStop, LiveStatsFn fn, const std::wstring& stateDir,
                    const std::wstring& stressPath) {
    NetArg* a = new NetArg();
    a->hStop    = hStop;
    a->fn       = fn;
    a->stateDir = stateDir;
    a->stressPath = stressPath;
    g_stressPathForNet = stressPath;
    HANDLE th = CreateThread(NULL, 0, NetThread, a, 0, NULL);
    if (th) CloseHandle(th);   // detached; exits on hStop
    else delete a;             // 线程创建失败时回收参数，避免泄漏
}
