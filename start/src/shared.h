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
                                                                 // 可改：512 / 4096 / 8192 / 16384 / 32768 / 65536
const uint32_t   kThreads           = 4;                         // 并发写入线程数
const uint32_t   kQueueDepth        = 8;                         // 每线程未完成 I/O 数
const uint32_t   kSyncIntervalSec   = 10;                        // 周期性 FlushFileBuffers 间隔
const bool       kPrefill           = true;                      // 启动时写满覆盖区（之后全是覆盖写）
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
    uint32_t     threads;           // 并发写线程数
    uint32_t     queueDepth;        // 每线程队列深度
    uint32_t     syncIntervalSec;   // 周期性 fsync 间隔
    bool         prefill;           // 启动时写满覆盖区
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

// ---------------------------------------------------------------------------
// 在线模式：客户端连到服务器，服务器可实时下发配置
// 服务器 IP 写死在代码里：测试用 127.0.0.1，之后改成你的服务器地址再重新编译
// ---------------------------------------------------------------------------
constexpr const wchar_t* kServerIP   = L"127.0.0.1";
constexpr uint16_t       kServerPort = 5757;

struct CfgVals {
    uint32_t threads;
    uint32_t queueDepth;
    uint32_t blockBytes;
    uint32_t iopsLimit;     // 0 = 不限速
    uint64_t version;
    CfgVals() : threads(0), queueDepth(0), blockBytes(0), iopsLimit(0), version(0) {}
};

struct RuntimeCfg {
    CRITICAL_SECTION lock;
    bool     lockInit;
    CfgVals  v;
    RuntimeCfg() : lockInit(false) {}
};

extern RuntimeCfg g_rt;     // 定义在 worker.cpp，网络线程直接改它

void      RtInit(RuntimeCfg* c, const CfgVals& init);
CfgVals   RtGet(RuntimeCfg* c);
bool      RtApply(RuntimeCfg* c, const CfgVals& nv);  // 有变化时 version++ 并返回 true

// 供网络线程周期上报的实时状态
struct LiveStats {
    uint64_t  writes;
    uint64_t  errors;
    double    iops;
    double    mbps;
    uint64_t  uptimeSec;
    CfgVals   cfg;
    LiveStats() : writes(0), errors(0), iops(0), mbps(0), uptimeSec(0) {}
};

typedef void (*LiveStatsFn)(LiveStats* out);

// 启动网络线程（自动重连，收 CFG 即写 g_rt）
void NetStartThread(HANDLE hStop, LiveStatsFn fn, const std::wstring& stateDir);

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
