#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#define APP_NAME        L"DiskStress"
#define APP_VER         L"1.0.0"
#define STOP_EVENT_GLOBAL L"Global\\DiskStress_StopEvent_v1"   // service (session 0)
#define STOP_EVENT_LOCAL  L"DiskStress_StopEvent_v1"           // interactive run
#define SERVICE_NAME      L"DiskStressService"
#define SERVICE_DISPLAY   L"DiskStress 4KiB Random Write Stress"
#define MUTEX_NAME      L"DiskStress_SingleInstance_v1"
#define REPORT_PREFIX   L"DiskStress_Report_"
#define REPORT_EXT      L".txt"
#define PID_FILE        L"worker.pid"
#define LOG_FILE        L"stop.log"
#define INI_FILE        L"DiskStress.ini"

struct Config {
    std::wstring stateDir;          // C:\ProgramData\DiskStress
    std::wstring filePath;          // stress data file
    std::wstring reportDir;         // report output dir
    uint64_t     workingSetBytes;   // logical span of random offsets (exceeds SSD cache)
    uint64_t     blockBytes;        // 4096
    uint64_t     physCapBytes;      // real usage ceiling; reached -> delete file and recreate
    uint32_t     reportIntervalSec; // 18000 = 5h
    uint32_t     segmentSec;        // 600 = 10min
    bool         noBuffering;       // FILE_FLAG_NO_BUFFERING
    bool         deleteOnExit;      // delete the stress file when the worker stops
};

// ---- paths & config ----
std::wstring GetExeDir();
Config      LoadConfig(const std::wstring& exeDir);
bool        EnsureDir(const std::wstring& dir);

// ---- disk footprint control ----
// Real on-disk allocated size of an open file, 0 on failure.
uint64_t GetFileAllocatedBytes(HANDLE hFile);

// ---- formatting ----
std::wstring FormatTime(const SYSTEMTIME& st);      // yyyy-MM-dd HH:mm:ss
std::wstring FormatStamp(const SYSTEMTIME& st);     // yyyyMMdd_HHmmss
std::wstring FormatBytes(uint64_t bytes);           // 8.00 GiB
std::wstring FormatDuration(double seconds);        // 05:00:12
std::wstring FormatInt(uint64_t v);                 // 1,234,567
std::wstring FormatDouble(double v, int digits);    // fixed digits

// ---- io helpers ----
void AppendLog(const std::wstring& dir, const std::wstring& line);
bool WriteTextFileUtf8(const std::wstring& path, const std::wstring& text);

// ---- timing ----
struct QpcClock {
    LARGE_INTEGER freq;
    void Init() { QueryPerformanceFrequency(&freq); }
    void Now(LARGE_INTEGER& out) const { QueryPerformanceCounter(&out); }
    double ElapsedSec(const LARGE_INTEGER& a, const LARGE_INTEGER& b) const {
        return (double)(b.QuadPart - a.QuadPart) / (double)freq.QuadPart;
    }
    double ElapsedUs(const LARGE_INTEGER& a, const LARGE_INTEGER& b) const {
        return (double)(b.QuadPart - a.QuadPart) * 1000000.0 / (double)freq.QuadPart;
    }
};
