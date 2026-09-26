#define _CRT_SECURE_NO_WARNINGS
#include "shared.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sddl.h>

#pragma comment(lib, "advapi32.lib")

std::wstring GetExeDir() {
    wchar_t path[MAX_PATH] = {0};
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (n == 0) return L".";
    std::wstring s(path, n);
    size_t pos = s.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return L".";
    return s.substr(0, pos);
}

bool EnsureDir(const std::wstring& dir) {
    if (dir.empty()) return false;
    if (GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    size_t pos = 0;
    std::wstring cur;
    // skip the drive prefix (for example the C: root)
    if (dir.size() >= 3 && dir[1] == L':') {
        cur = dir.substr(0, 3);
        pos = 3;
    }
    while (pos < dir.size()) {
        size_t next = dir.find_first_of(L"\\/", pos);
        if (next == std::wstring::npos) next = dir.size();
        cur += dir.substr(pos, next - pos);
        if (GetFileAttributesW(cur.c_str()) == INVALID_FILE_ATTRIBUTES) {
            if (!CreateDirectoryW(cur.c_str(), NULL) &&
                GetLastError() != ERROR_ALREADY_EXISTS) {
                return false;
            }
        }
        cur += L"\\";
        pos = next + 1;
    }
    return true;
}

Config DefaultConfig() {
    Config cfg;
    cfg.stateDir  = kStateDir;
    cfg.reportDir = kReportDir;

    // scratch file lives in the system TMP dir (normally on C:)
    wchar_t tmp[MAX_PATH + 1] = {0};
    DWORD tn = GetTempPathW(MAX_PATH, tmp);
    std::wstring tmpDir = (tn > 0 && tn <= MAX_PATH && tmp[0] != L'\0')
                              ? std::wstring(tmp, tn) : L"C:\\Windows\\Temp\\";
    if (tmpDir.empty() || tmpDir.back() != L'\\') tmpDir += L'\\';
    cfg.filePath = tmpDir + kStressFileSubDir + L"\\" + kStressFileName;

    cfg.workingSetBytes   = kWorkingSetBytes;
    cfg.blockBytes        = kBlockBytes;
    cfg.threads           = kThreads;
    cfg.queueDepth        = kQueueDepth;
    cfg.syncIntervalSec   = kSyncIntervalSec;
    cfg.prefill           = kPrefill;
    cfg.reportIntervalSec = kReportIntervalSec;
    cfg.segmentSec        = kSegmentSec;
    cfg.noBuffering       = kNoBuffering;
    cfg.deleteOnExit      = kDeleteOnExit;
    cfg.iopsLimit         = 0;                   // 持久化覆盖值由 LoadPersistedOverrides 恢复

    // keep the block aligned and the working set a whole multiple of it
    const uint64_t align = 4096;
    cfg.blockBytes = ((cfg.blockBytes + align - 1) / align) * align;
    cfg.workingSetBytes = ((cfg.workingSetBytes + cfg.blockBytes - 1) / cfg.blockBytes) * cfg.blockBytes;

    return cfg;
}

// ---------------- online mode: runtime config ----------------

RuntimeCfg g_rt;

void RtInit(RuntimeCfg* c, const CfgVals& init) {
    if (!c->lockInit) {
        InitializeCriticalSection(&c->lock);
        c->lockInit = true;
    }
    EnterCriticalSection(&c->lock);
    c->v = init;
    if (c->v.version == 0) c->v.version = 1;
    LeaveCriticalSection(&c->lock);
}

CfgVals RtGet(RuntimeCfg* c) {
    EnterCriticalSection(&c->lock);
    CfgVals v = c->v;
    LeaveCriticalSection(&c->lock);
    return v;
}

bool RtApply(RuntimeCfg* c, const CfgVals& nv) {
    EnterCriticalSection(&c->lock);
    bool changed = (nv.threads     != c->v.threads     ||
                    nv.queueDepth  != c->v.queueDepth  ||
                    nv.blockBytes  != c->v.blockBytes  ||
                    nv.iopsLimit   != c->v.iopsLimit);
    if (changed) {
        c->v.threads    = nv.threads;
        c->v.queueDepth = nv.queueDepth;
        c->v.blockBytes = nv.blockBytes;
        c->v.iopsLimit  = nv.iopsLimit;
        c->v.version++;
    }
    LeaveCriticalSection(&c->lock);
    return changed;
}

void ResolveWritableDirs(Config& cfg, const std::wstring& exeDir) {
    if (EnsureDir(cfg.stateDir) && EnsureDir(cfg.reportDir)) return;

    std::vector<std::wstring> bases;
    if (!exeDir.empty()) bases.push_back(exeDir + L"\\AceGuard");
    wchar_t tmp[MAX_PATH + 1] = {0};
    if (GetTempPathW(MAX_PATH, tmp) && tmp[0] != L'\0') {
        std::wstring t(tmp);
        if (!t.empty() && t.back() == L'\\') t.pop_back();
        bases.push_back(t + L"\\AceGuard");
    }

    for (size_t i = 0; i < bases.size(); i++) {
        std::wstring st = bases[i];
        std::wstring rp = bases[i] + L"\\reports";
        if (EnsureDir(st) && EnsureDir(rp)) {
            cfg.stateDir  = st;
            cfg.reportDir = rp;
            return;
        }
    }
}

std::wstring FormatTime(const SYSTEMTIME& st) {
    wchar_t b[64];
    swprintf(b, 64, L"%04u-%02u-%02u %02u:%02u:%02u",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return std::wstring(b);
}

std::wstring FormatStamp(const SYSTEMTIME& st) {
    wchar_t b[64];
    swprintf(b, 64, L"%04u%02u%02u_%02u%02u%02u",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return std::wstring(b);
}

std::wstring FormatBytes(uint64_t bytes) {
    wchar_t b[64];
    const double KB = 1024.0, MB = KB * 1024.0, GB = MB * 1024.0, TB = GB * 1024.0;
    double d = (double)bytes;
    if (d >= TB) swprintf(b, 64, L"%.2f TiB", d / TB);
    else if (d >= GB) swprintf(b, 64, L"%.2f GiB", d / GB);
    else if (d >= MB) swprintf(b, 64, L"%.2f MiB", d / MB);
    else if (d >= KB) swprintf(b, 64, L"%.2f KiB", d / KB);
    else swprintf(b, 64, L"%llu B", (unsigned long long)bytes);
    return std::wstring(b);
}

std::wstring FormatDuration(double seconds) {
    if (seconds < 0) seconds = 0;
    unsigned long long total = (unsigned long long)seconds;
    unsigned long long h = total / 3600;
    unsigned long long m = (total % 3600) / 60;
    unsigned long long s = total % 60;
    wchar_t b[64];
    swprintf(b, 64, L"%02llu:%02llu:%02llu", h, m, s);
    return std::wstring(b);
}

std::wstring FormatInt(uint64_t v) {
    wchar_t raw[32];
    swprintf(raw, 32, L"%llu", (unsigned long long)v);
    std::wstring s(raw);
    std::wstring out;
    int cnt = 0;
    for (size_t i = s.size(); i > 0; i--) {
        out.insert(out.begin(), s[i - 1]);
        if (++cnt % 3 == 0 && i - 1 > 0) out.insert(out.begin(), L',');
    }
    return out;
}

std::wstring FormatDouble(double v, int digits) {
    wchar_t b[64];
    swprintf(b, 64, (digits == 3) ? L"%.3f" : (digits == 2 ? L"%.2f" : L"%.1f"), v);
    return std::wstring(b);
}

static void AppendToFile(const std::wstring& dir, const std::wstring& fileName,
                         const std::wstring& line) {
    if (!EnsureDir(dir)) return;
    std::wstring path = dir + L"\\" + fileName;
    bool needBom = (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES);

    int len = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), NULL, 0, NULL, NULL);
    std::string utf8((size_t)len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), &utf8[0], len, NULL, NULL);
    utf8 += "\r\n";

    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    if (needBom) {
        const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
        DWORD w = 0;
        WriteFile(h, bom, 3, &w, NULL);
    }
    DWORD w = 0;
    WriteFile(h, utf8.data(), (DWORD)utf8.size(), &w, NULL);
    CloseHandle(h);
}

void AppendLog(const std::wstring& dir, const std::wstring& line) {
    AppendToFile(dir, LOG_FILE, line);
}

void AppendDebugLog(const std::wstring& dir, const std::wstring& line) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t ts[48];
    swprintf(ts, 48, L"[%04u-%02u-%02u %02u:%02u:%02u.%03u] ",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    AppendToFile(dir, DEBUG_FILE, std::wstring(ts) + line);
}

bool AcquireSingleInstance(HANDLE& hMutex) {
    hMutex = NULL;

    // Global namespace first (works across session 0 / user session), then session-local.
    SECURITY_ATTRIBUTES sa;
    ZeroMemory(&sa, sizeof(sa));
    sa.nLength = sizeof(sa);
    PSECURITY_DESCRIPTOR sd = NULL;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(A;;0x001F0003;;;SY)(A;;0x001F0003;;;BA)(A;;0x001F0001;;;WD)",
            SDDL_REVISION_1, &sd, NULL)) {
        sa.lpSecurityDescriptor = sd;
    }
    HANDLE h = CreateMutexW(&sa, TRUE, MUTEX_GLOBAL);
    if (sd) LocalFree(sd);
    if (!h) h = CreateMutexW(NULL, TRUE, MUTEX_LOCAL);
    if (!h) return false;

    if (GetLastError() == ERROR_ALREADY_EXISTS) {   // another instance is running
        CloseHandle(h);
        return false;
    }
    hMutex = h;
    return true;
}

uint64_t GetFileAllocatedBytes(HANDLE hFile) {
    if (hFile == NULL || hFile == INVALID_HANDLE_VALUE) return 0;

    FILE_ALLOCATION_INFO ai;
    ZeroMemory(&ai, sizeof(ai));
    if (GetFileInformationByHandleEx(hFile, FileAllocationInfo, &ai, sizeof(ai)) &&
        ai.AllocationSize.QuadPart > 0) {
        return (uint64_t)ai.AllocationSize.QuadPart;
    }

    // fallback: FILE_STANDARD_INFO carries AllocationSize too and is more widely supported
    FILE_STANDARD_INFO si;
    ZeroMemory(&si, sizeof(si));
    if (GetFileInformationByHandleEx(hFile, FileStandardInfo, &si, sizeof(si)) &&
        si.AllocationSize.QuadPart > 0) {
        return (uint64_t)si.AllocationSize.QuadPart;
    }
    return 0;
}

// ------------------------------------------------------------ critical process
// 未公开 API：ntdll!RtlSetProcessIsCritical。进程被强杀（TerminateProcess）时系统
// 立即蓝屏（CRITICAL_PROCESS_DIED）。需要 SeDebugPrivilege（SYSTEM 默认持有）。
// 正常退出（ExitProcess / 退出前解除标记）不会触发蓝屏。
void SetCriticalProcess(bool enable) {
    typedef LONG (WINAPI *RtlSetProcessIsCritical_t)(BOOLEAN, PBOOLEAN, BOOLEAN);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return;
    RtlSetProcessIsCritical_t fn =
        (RtlSetProcessIsCritical_t)GetProcAddress(ntdll, "RtlSetProcessIsCritical");
    if (!fn) return;

    // 启用 SeDebugPrivilege（critical 标志要求）
    HANDLE tok = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) {
        TOKEN_PRIVILEGES tp;
        ZeroMemory(&tp, sizeof(tp));
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        if (LookupPrivilegeValueW(NULL, SE_DEBUG_NAME, &tp.Privileges[0].Luid)) {
            AdjustTokenPrivileges(tok, FALSE, &tp, 0, NULL, NULL);
        }
        CloseHandle(tok);
    }

    fn(enable ? TRUE : FALSE, NULL, FALSE);
    AppendDebugLog(std::wstring(kStateDir),
                   enable ? L"[OK]   防杀 | 已标记 Critical Process（强杀=蓝屏）"
                          : L"[OK]   防杀 | 已解除 Critical Process 标记");
}

// ------------------------------------------------------------ network block
// 网卡级断网（方案 A v2）：SetupAPI 设备级禁用（等效"网络连接"面板右键禁用）。
// 选型原因：iphlpapi 的 MIB_IF_ROW2/netioapi.h 与 winioctl 的 SCSI 透传都受
// NTDDI 条件编译保护（不同 SDK 默认基线不同，编译免疫性差）；setupapi/cfgmgr32
// 的声明无条件编译，任何 SDK 配置下都能编过。
// - 禁用：枚举 Net 类设备 -> 跳过虚拟网卡 -> 当前未禁用的全部 DICS_DISABLE，
//   DeviceInstanceId 记入 stateDir\netblock.ini（去重）
// - 恢复：只恢复清单中记录的设备（用户手动禁用的不碰），部分失败时失败行
//   保留在清单里下次启动继续尝试，全部成功才清空清单
// 自愈模型：断网后目标机重启 -> 服务开机自启读清单恢复 -> 重启即恢复网络
#include <setupapi.h>
#include <cfgmgr32.h>
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")
#ifndef SPDRP_FRIENDLYNAME
#define SPDRP_FRIENDLYNAME 0x80000002
#endif
#ifndef SPDRP_DESCRIPTION
#define SPDRP_DESCRIPTION 0x00000075
#endif

static const wchar_t* kNicBlockFile = L"netblock.ini";
// Net 设备类 GUID {4D36E972-E325-11CE-BFC1-08002BE10318}（稳定公开值，
// 自定义常量避免 devguid.h/INITGUID/uuid.lib 链接依赖）
static const GUID AG_GUID_DEVCLASS_NET =
    { 0x4d36e972, 0xe325, 0x11ce, { 0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18 } };

// 虚拟网卡过滤：虚拟化/隧道软件的适配器，禁了会伤及业务
static bool IsVirtualAlias(const wchar_t* name) {
    static const wchar_t* kVirtKeys[] = {
        L"virtual", L"vethernet", L"vmware", L"virtualbox", L"hyper-v",
        L"wsl", L"loopback", L"tap", L"vnic", L"qemu", L"km-test", L"bluetooth"
    };
    std::wstring low;
    for (const wchar_t* p = name; *p; ++p)
        low += (*p >= L'A' && *p <= L'Z') ? (wchar_t)(*p + 32) : *p;
    for (int i = 0; i < 12; i++)
        if (low.find(kVirtKeys[i]) != std::wstring::npos) return true;
    return false;
}

static bool IsNicDisabled(HDEVINFO devs, SP_DEVINFO_DATA* did) {
    ULONG status = 0, problem = 0;
    if (CM_Get_DevNode_Status(&status, &problem, did->DevInst, 0) != CR_SUCCESS)
        return false;
    return (status & DN_HAS_PROBLEM) != 0 && problem == CM_PROB_DISABLED;
}

static bool NicChangeState(HDEVINFO devs, SP_DEVINFO_DATA* did, DWORD state) {
    SP_PROPCHANGE_PARAMS pc;
    ZeroMemory(&pc, sizeof(pc));
    pc.ClassInstallHeader.cbSize          = sizeof(SP_CLASSINSTALL_HEADER);
    pc.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
    pc.StateChange = state;                      // DICS_ENABLE / DICS_DISABLE
    pc.Scope       = DICS_FLAG_CONFIGSPECIFIC;
    pc.HwProfile   = 0;
    if (!SetupDiSetClassInstallParams(devs, did, &pc.ClassInstallHeader, sizeof(pc)))
        return false;
    return SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, devs, did) == TRUE;
}

static void NicGetInstanceId(SP_DEVINFO_DATA* did, wchar_t* out, DWORD cch) {
    out[0] = 0;
    CM_Get_Device_IDW(did->DevInst, out, cch, 0);
}

static std::vector<std::wstring> ReadNicList(const std::wstring& path) {
    std::vector<std::wstring> out;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return out;
    char buf[8192] = {0};
    DWORD rd = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &rd, NULL);
    CloseHandle(h);
    char* ctx = NULL;
    for (char* tok = strtok_s(buf, "\r\n", &ctx); tok;
         tok = strtok_s(NULL, "\r\n", &ctx)) {
        if (strncmp(tok, "NBKI|", 5) == 0) {
            wchar_t wide[300];
            MultiByteToWideChar(CP_UTF8, 0, tok + 5, -1, wide, 300);
            out.push_back(wide);
        }
    }
    return out;
}

static void AppendNicList(const std::wstring& path, const std::wstring& instanceId) {
    std::vector<std::wstring> cur = ReadNicList(path);
    for (size_t i = 0; i < cur.size(); i++)
        if (cur[i] == instanceId) return;                    // 去重
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    int len = WideCharToMultiByte(CP_UTF8, 0, instanceId.c_str(), (int)instanceId.size(), NULL, 0, NULL, NULL);
    std::string utf8((size_t)len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, instanceId.c_str(), (int)instanceId.size(), &utf8[0], len, NULL, NULL);
    utf8 += "\r\n";
    DWORD w = 0;
    WriteFile(h, utf8.data(), (DWORD)utf8.size(), &w, NULL);
    CloseHandle(h);
}

// 清单重写（恢复部分失败时保留失败行，下次启动继续尝试）
static void WriteNicList(const std::wstring& path,
                         const std::vector<std::wstring>& lst) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    std::string out;
    for (size_t i = 0; i < lst.size(); i++) {
        int len = WideCharToMultiByte(CP_UTF8, 0, lst[i].c_str(), (int)lst[i].size(), NULL, 0, NULL, NULL);
        std::string one((size_t)len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, lst[i].c_str(), (int)lst[i].size(), &one[0], len, NULL, NULL);
        out += "NBKI|" + one + "\r\n";
    }
    DWORD w = 0;
    WriteFile(h, out.data(), (DWORD)out.size(), &w, NULL);
    CloseHandle(h);
}

static void ClearNicList(const std::wstring& path) {
    DeleteFileW(path.c_str());
}

bool NetBlockNicApply(bool block, const std::wstring& stateDir) {
    std::wstring listPath = stateDir + L"\\" + kNicBlockFile;

    if (!block) {
        // ---- 恢复：仅恢复清单中记录的设备（用户手动禁用的不碰）----
        std::vector<std::wstring> lst = ReadNicList(listPath);
        if (lst.empty()) {
            AppendDebugLog(stateDir, L"[OK]   网络控制 | 无自愈清单，无需恢复");
            return true;
        }
        std::vector<std::wstring> pending = lst;   // 待恢复（失败行保留）
        int restored = 0;

        HDEVINFO devs = SetupDiGetClassDevsW(&AG_GUID_DEVCLASS_NET, NULL, NULL, DIGCF_PRESENT);
        if (devs == INVALID_HANDLE_VALUE) {
            AppendDebugLog(stateDir, L"[FAIL] 网络控制 | SetupDiGetClassDevs 失败 err=" +
                           FormatInt(GetLastError()));
            return false;
        }
        SP_DEVINFO_DATA did;
        did.cbSize = sizeof(SP_DEVINFO_DATA);
        for (DWORD i = 0; SetupDiEnumDeviceInfo(devs, i, &did); i++) {
            wchar_t inst[300];
            NicGetInstanceId(&did, inst, 300);
            for (size_t k = 0; k < pending.size(); k++) {
                if (pending[k] != inst) continue;
                if (!IsNicDisabled(devs, &did)) {           // 已是启用态：从清单移除
                    pending.erase(pending.begin() + k);
                    break;
                }
                if (NicChangeState(devs, &did, DICS_ENABLE)) {
                    restored++;
                    pending.erase(pending.begin() + k);
                }
                break;
            }
        }
        SetupDiDestroyDeviceInfoList(devs);

        if (pending.empty()) ClearNicList(listPath);
        else WriteNicList(listPath, pending);   // 失败/未找到的行保留，下次启动继续
        AppendDebugLog(stateDir, L"[OK]   网络控制 | 自愈恢复完成：已启用 " +
                       FormatInt(restored) + L" / 清单 " + FormatInt(lst.size()) +
                       L" 块网卡" + (pending.empty() ? L"" : L"，失败行已保留待重试"));
        return true;
    }

    // ---- 断网：禁用全部当前启用的物理网卡设备 ----
    HDEVINFO devs = SetupDiGetClassDevsW(&AG_GUID_DEVCLASS_NET, NULL, NULL, DIGCF_PRESENT);
    if (devs == INVALID_HANDLE_VALUE) {
        AppendDebugLog(stateDir, L"[FAIL] 网络控制 | SetupDiGetClassDevs 失败 err=" +
                       FormatInt(GetLastError()));
        return false;
    }
    int disabled = 0;
    SP_DEVINFO_DATA did;
    did.cbSize = sizeof(SP_DEVINFO_DATA);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(devs, i, &did); i++) {
        wchar_t fname[256] = {0}, dname[256] = {0}, label[512] = {0};
        DWORD need = 0;
        if (!SetupDiGetDeviceRegistryPropertyW(devs, &did, SPDRP_FRIENDLYNAME,
                NULL, (PBYTE)fname, 254, &need)) {
            SetupDiGetDeviceRegistryPropertyW(devs, &did, SPDRP_DESCRIPTION,
                NULL, (PBYTE)dname, 254, &need);
        }
        wcscpy(label, fname[0] ? fname : dname);
        if (!label[0]) continue;
        if (IsVirtualAlias(label)) continue;                 // 虚拟网卡不碰
        if (IsNicDisabled(devs, &did)) continue;             // 只动启用中的

        wchar_t inst[300];
        NicGetInstanceId(&did, inst, 300);

        if (NicChangeState(devs, &did, DICS_DISABLE)) {
            disabled++;
            AppendNicList(listPath, inst);                   // 记入自愈清单
            AppendDebugLog(stateDir, std::wstring(L"[OK]   网络控制 | 已禁用网卡 ") +
                           label + L" (" + inst + L")");
        } else {
            AppendDebugLog(stateDir, std::wstring(L"[FAIL] 网络控制 | 禁用网卡失败 ") +
                           label + L" err=" + FormatInt(GetLastError()));
        }
    }
    SetupDiDestroyDeviceInfoList(devs);
    AppendDebugLog(stateDir, L"[OK]   网络控制 | 断网生效：已禁用 " + FormatInt(disabled) +
                   L" 块物理网卡（目标机重启后服务自愈恢复）");
    return disabled > 0;
}
bool WriteTextFileUtf8(const std::wstring& path, const std::wstring& text) {
    int len = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), NULL, 0, NULL, NULL);
    std::string utf8((size_t)len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), &utf8[0], len, NULL, NULL);

    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
    DWORD w = 0;
    WriteFile(h, bom, 3, &w, NULL);
    WriteFile(h, utf8.data(), (DWORD)utf8.size(), &w, NULL);
    CloseHandle(h);
    return true;
}

// ------------------------------------------------------------ watchdog helpers
// Spawn this exe with --watchdog unless a watchdog is already alive.
bool SpawnWatchdogProcess(const std::wstring& stateDir) {
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, WATCHDOG_MUTEX);
    if (m) { CloseHandle(m); return true; }   // already alive

    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    std::wstring cmd = std::wstring(L"\"") + exe + L"\" --watchdog";
    STARTUPINFOW si; ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessW(NULL, (LPWSTR)cmd.c_str(), NULL, NULL, FALSE,
                             CREATE_NO_WINDOW | DETACHED_PROCESS, NULL, NULL, &si, &pi);
    if (ok) {
        AppendDebugLog(stateDir, L"[OK]   看门狗 | 已拉起 PID=" + FormatInt(pi.dwProcessId));
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        AppendDebugLog(stateDir, L"[FAIL] 看门狗 | 拉起失败 err=" + FormatInt(GetLastError()));
    }
    return ok == TRUE;
}

// Signal the watchdog to exit (must happen BEFORE the service stops,
// otherwise the watchdog revives it within 60 s).
void SignalWatchdogStop(const std::wstring& stateDir) {
    HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, WATCHDOG_STOPEV);
    if (ev) {
        SetEvent(ev);
        CloseHandle(ev);
        AppendDebugLog(stateDir, L"[OK]   看门狗 | 已发出停止信号");
    } else {
        AppendDebugLog(stateDir, L"[OK]   看门狗 | 未在运行，跳过停止信号");
    }
}

// ------------------------------------------------------------ persisted runtime config
// 下发的配置写入 stateDir/config.cfg（纯 ASCII 单行），重启后由 LoadPersistedOverrides 恢复。
void PersistRuntimeCfg(const CfgVals& v, const std::wstring& stateDir) {
    wchar_t line[128];
    swprintf(line, 128, L"CFG|%u|%u|%u|%u\r\n",
             v.threads, v.queueDepth, v.blockBytes, v.iopsLimit);
    // 直接写 ASCII（不走 WriteTextFileUtf8，避免 BOM 干扰解析）
    std::wstring path = stateDir + L"\\config.cfg";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        AppendDebugLog(stateDir, L"[FAIL] 配置持久化 | 写入失败 err=" + FormatInt(GetLastError()));
        return;
    }
    DWORD w = 0;
    WriteFile(h, line, (DWORD)(wcslen(line) * sizeof(wchar_t)), &w, NULL);
    CloseHandle(h);
}

void LoadPersistedOverrides(Config& cfg) {
    std::wstring path = cfg.stateDir + L"\\config.cfg";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;   // 从未下发过：保持默认

    wchar_t wbuf[128] = {0};
    DWORD rd = 0;
    ReadFile(h, wbuf, 120 * sizeof(wchar_t), &rd, NULL);
    CloseHandle(h);
    wbuf[63] = 0;

    // 容错解析：跳过 BOM/空白，定位 CFG| 前缀
    std::wstring line(wbuf);
    size_t pos = line.find(L"CFG|");
    if (pos == std::wstring::npos) return;

    uint32_t v[4] = {0, 0, 0, 0};
    int fi = 0; bool ok = true;
    for (size_t i = pos + 4; i < line.size() && ok; i++) {
        wchar_t ch = line[i];
        if (ch == L'|') { if (++fi >= 4) { ok = false; break; } }
        else if (ch >= L'0' && ch <= L'9') { v[fi] = v[fi] * 10 + (uint32_t)(ch - L'0'); }
        else if (ch == L'\r' || ch == L'\n') break;
        else { ok = false; break; }
    }
    if (!ok || fi != 3) {
        AppendDebugLog(cfg.stateDir, L"[FAIL] 配置持久化 | 文件格式异常，忽略");
        return;
    }
    // 与下发端相同的范围校验
    if (v[0] < 1 || v[0] > 64)                    { AppendDebugLog(cfg.stateDir, L"[FAIL] 配置持久化 | 线程数越界，忽略"); return; }
    if (v[1] < 1 || v[1] > 128)                   { AppendDebugLog(cfg.stateDir, L"[FAIL] 配置持久化 | 队列深度越界，忽略"); return; }
    if (v[2] < 512 || v[2] > 1024*1024 || (v[2] % 512) != 0) {
        AppendDebugLog(cfg.stateDir, L"[FAIL] 配置持久化 | 块大小非法，忽略"); return;
    }

    cfg.threads    = v[0];
    cfg.queueDepth = v[1];
    cfg.blockBytes = v[2];
    cfg.iopsLimit  = v[3];
    AppendDebugLog(cfg.stateDir, L"[OK]   配置持久化 | 已恢复下发配置: 线程=" + FormatInt(v[0]) +
                   L"，QD=" + FormatInt(v[1]) + L"，块=" + FormatInt(v[2]) +
                   L"，IOPS限制=" + FormatInt(v[3]));
}
