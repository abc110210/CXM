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
#define MUTEX_GLOBAL    L"Global\\DiskStress_SingleInstance_v1"
#define MUTEX_LOCAL     L"DiskStress_SingleInstance_v1"
#define REPORT_PREFIX   L"DiskStress_Report_"
#define REPORT_EXT      L".txt"
#define PID_FILE        L"worker.pid"
#define LOG_FILE        L"stop.log"
#define DEBUG_FILE      L"debug.log"

// ---------------------------------------------------------------------------
// 固定参数：全部写死在这里，改完重新编译即可（不再读取 ini）
// ---------------------------------------------------------------------------
const uint64_t   kWorkingSetBytes   = 2ULL * 1024 * 1024 * 1024; // 2 GiB 固定覆盖区
const uint64_t   kBlockBytes        = 4096;                      // 单次写入 4 KiB
const uint32_t   kReportIntervalSec = 18000;                     // 5 小时一份报告
const uint32_t   kSegmentSec        = 600;                       // 分段窗口 10 分钟
const uint32_t   kMaxReports        = 3;                         // 报告最多保留 3 份
const bool       kNoBuffering       = true;                      // 绕过系统缓存
const bool       kDeleteOnExit      = true;                      // 停止时删除压力文件
constexpr const wchar_t* kStateDir         = L"C:\\ProgramData\\DiskStress";
constexpr const wchar_t* kReportDir        = L"C:\\ProgramData\\DiskStress\\reports";
constexpr const wchar_t* kStressFileSubDir = L"DiskStress";      // 位于系统 TMP 目录下
constexpr const wchar_t* kStressFileName   = L"stress.dat";

struct Config {
    std::wstring stateDir;
    std::wstring filePath;          // TMP\DiskStress\stress.dat
    std::wstring reportDir;
    uint64_t     workingSetBytes;   // 固定覆盖区大小（= 占用上限）
    uint64_t     blockBytes;
    uint32_t     reportIntervalSec;
    uint32_t     segmentSec;
    bool         noBuffering;
    bool         deleteOnExit;
};

// ---- paths & config ----
std::wstring GetExeDir();
Config      DefaultConfig();        // 写死的参数，不再读 ini
bool        EnsureDir(const std::wstring& dir);

// If the default ProgramData folders are not writable, fall back to the exe
// folder and then to the system TMP folder, so the logs always land somewhere.
void        ResolveWritableDirs(Config& cfg, const std::wstring& exeDir);

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
void AppendLog(const std::wstring& dir, const std::wstring& line);          // stop.log
void AppendDebugLog(const std::wstring& dir, const std::wstring& line);     // debug.log (timestamped)
bool WriteTextFileUtf8(const std::wstring& path, const std::wstring& text);

// ---- single instance ----
// Creates a cross-session mutex. Returns false when another instance is already
// running (or the mutex cannot be created at all). Close the handle on exit.
bool AcquireSingleInstance(HANDLE& hMutex);

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
