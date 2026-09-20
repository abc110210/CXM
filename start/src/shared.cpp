#include "shared.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwchar>

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

static uint64_t ParseUInt(const std::wstring& s, uint64_t defVal) {
    if (s.empty()) return defVal;
    uint64_t v = 0;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] < L'0' || s[i] > L'9') return defVal;
        v = v * 10 + (uint64_t)(s[i] - L'0');
    }
    return v;
}

static double ParseDouble(const std::wstring& s, double defVal) {
    if (s.empty()) return defVal;
    double v = _wtof(s.c_str());
    if (v <= 0) return defVal;
    return v;
}

Config LoadConfig(const std::wstring& exeDir) {
    Config cfg;
    cfg.stateDir          = L"C:\\ProgramData\\DiskStress";
    cfg.reportDir         = L"C:\\ProgramData\\DiskStress\\reports";

    // default scratch file lives in the system TMP dir (normally on C:)
    {
        wchar_t tmp[MAX_PATH + 1] = {0};
        DWORD tn = GetTempPathW(MAX_PATH, tmp);
        std::wstring tmpDir = (tn > 0 && tn <= MAX_PATH && tmp[0] != L'\0')
                                  ? std::wstring(tmp, tn) : L"C:\\Windows\\Temp\\";
        if (tmpDir.empty() || tmpDir.back() != L'\\') tmpDir += L'\\';
        cfg.filePath = tmpDir + L"DiskStress\\stress.dat";
    }

    cfg.workingSetBytes   = (uint64_t)10 * 1024 * 1024 * 1024; // 10 GiB logical span
    cfg.blockBytes        = 4096;
    cfg.physCapBytes      = (uint64_t)10 * 1024 * 1024 * 1024; // 10 GiB ceiling
    cfg.reportIntervalSec = 18000; // 5 hours
    cfg.segmentSec        = 600;   // 10 minutes
    cfg.noBuffering       = true;
    cfg.deleteOnExit      = true;

    std::wstring ini = exeDir + L"\\" + INI_FILE;
    wchar_t buf[1024];

    auto readStr = [&](const wchar_t* key, const std::wstring& def) -> std::wstring {
        DWORD n = GetPrivateProfileStringW(L"DiskStress", key, def.c_str(), buf, 1024, ini.c_str());
        return std::wstring(buf, n);
    };

    std::wstring v;

    v = readStr(L"StateDir", cfg.stateDir);   if (!v.empty()) cfg.stateDir = v;
    v = readStr(L"FilePath", cfg.filePath);   if (!v.empty()) cfg.filePath = v;
    v = readStr(L"ReportDir", cfg.reportDir); if (!v.empty()) cfg.reportDir = v;

    v = readStr(L"WorkingSetGiB", L"10");
    double gib = ParseDouble(v, 10.0);
    cfg.workingSetBytes = (uint64_t)(gib * 1024.0 * 1024.0 * 1024.0);

    cfg.blockBytes = (uint64_t)ParseUInt(readStr(L"BlockBytes", L"4096"), 4096);
    if (cfg.blockBytes < 512) cfg.blockBytes = 512;
    if (cfg.blockBytes > 1024 * 1024) cfg.blockBytes = 1024 * 1024;

    cfg.reportIntervalSec = (uint32_t)ParseUInt(readStr(L"ReportIntervalMinutes", L"300"), 300) * 60;
    if (cfg.reportIntervalSec < 60) cfg.reportIntervalSec = 60;

    cfg.segmentSec = (uint32_t)ParseUInt(readStr(L"SegmentMinutes", L"10"), 10) * 60;
    if (cfg.segmentSec < 10) cfg.segmentSec = 10;

    cfg.noBuffering = ParseUInt(readStr(L"NoBuffering", L"1"), 1) != 0;

    v = readStr(L"PhysicalCapGiB", L"10");
    double capGib = ParseDouble(v, 10.0);
    cfg.physCapBytes = (uint64_t)(capGib * 1024.0 * 1024.0 * 1024.0);

    cfg.deleteOnExit = ParseUInt(readStr(L"DeleteFileOnExit", L"1"), 1) != 0;

    // keep block aligned and working set a multiple of the block size
    uint64_t align = 4096;
    cfg.blockBytes = ((cfg.blockBytes + align - 1) / align) * align;
    cfg.workingSetBytes = ((cfg.workingSetBytes + cfg.blockBytes - 1) / cfg.blockBytes) * cfg.blockBytes;

    if (cfg.physCapBytes >= cfg.blockBytes) {
        cfg.physCapBytes = ((cfg.physCapBytes + cfg.blockBytes - 1) / cfg.blockBytes) * cfg.blockBytes;
    } else {
        cfg.physCapBytes = 0; // 0 = no cap, keep the full working set resident
    }
    if (cfg.physCapBytes > cfg.workingSetBytes) cfg.physCapBytes = cfg.workingSetBytes;

    return cfg;
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

void AppendLog(const std::wstring& dir, const std::wstring& line) {
    if (!EnsureDir(dir)) return;
    std::wstring path = dir + L"\\" + LOG_FILE;
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

uint64_t GetFileAllocatedBytes(HANDLE hFile) {
    if (hFile == NULL || hFile == INVALID_HANDLE_VALUE) return 0;
    FILE_ALLOCATION_INFO ai;
    ZeroMemory(&ai, sizeof(ai));
    if (GetFileInformationByHandleEx(hFile, FileAllocationInfo, &ai, sizeof(ai))) {
        return (uint64_t)ai.AllocationSize.QuadPart;
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
