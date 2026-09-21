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
    if (!exeDir.empty()) bases.push_back(exeDir + L"\\DiskStress");
    wchar_t tmp[MAX_PATH + 1] = {0};
    if (GetTempPathW(MAX_PATH, tmp) && tmp[0] != L'\0') {
        std::wstring t(tmp);
        if (!t.empty() && t.back() == L'\\') t.pop_back();
        bases.push_back(t + L"\\DiskStress");
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
