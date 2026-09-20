#include "worker.h"
#include "report.h"

#include <cstdio>
#include <cstring>
#include <malloc.h>
#include <random>

#pragma comment(lib, "kernel32.lib")

static const wchar_t* REASON_PERIODIC = L"周期报告（每 5 小时一份）";
static const wchar_t* REASON_STOPPED  = L"收到停止指令（DiskStress 控制台）";
static const wchar_t* REASON_ERROR    = L"连续 I/O 错误，程序自我保护退出";

int RunWorker(const Config& cfgIn, HANDLE hStopA, HANDLE hStopB, HANDLE hStopC) {
    Config cfg = cfgIn;

    if (!EnsureDir(cfg.stateDir))  return 2;
    if (!EnsureDir(cfg.reportDir)) return 2;

    // --- pid file (used by the stop console to wait for a clean exit) ---
    {
        wchar_t pid[32];
        swprintf(pid, 32, L"%lu", GetCurrentProcessId());
        WriteTextFileUtf8(cfg.stateDir + L"\\" + PID_FILE, std::wstring(pid));
    }

    // --- keep the system awake during the stress run ---
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);

    // --- never eat more than half of what is left on the volume ---
    {
        ULARGE_INTEGER freeBytes;
        std::wstring root = (cfg.filePath.size() >= 2 && cfg.filePath[1] == L':')
                                ? cfg.filePath.substr(0, 2) + L"\\" : L"C:\\";
        if (GetDiskFreeSpaceExW(root.c_str(), &freeBytes, NULL, NULL)) {
            uint64_t budget = (uint64_t)((double)freeBytes.QuadPart * 0.5);
            budget = (budget / cfg.blockBytes) * cfg.blockBytes;
            if (budget < cfg.workingSetBytes) {
                cfg.workingSetBytes = budget;
                AppendLog(cfg.stateDir, L"[worker] working set clamped by free disk space");
            }
        }
    }

    // --- sector size check for unbuffered I/O ---
    bool noBuf = cfg.noBuffering;
    {
        std::wstring root = (cfg.filePath.size() >= 2 && cfg.filePath[1] == L':')
                                ? cfg.filePath.substr(0, 2) + L"\\" : L"C:\\";
        DWORD spc = 0, bps = 0, nfc = 0, tnc = 0;
        if (GetDiskFreeSpaceW(root.c_str(), &spc, &bps, &nfc, &tnc)) {
            if (bps == 0) bps = 4096;
            if ((cfg.blockBytes % bps) != 0) noBuf = false;
        }
    }

    // --- open / create the stress file ---
    DWORD flags = FILE_FLAG_RANDOM_ACCESS;
    if (noBuf) flags |= FILE_FLAG_NO_BUFFERING;
    HANDLE hFile = CreateFileW(cfg.filePath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL,
                               OPEN_ALWAYS, flags, NULL);
    if (hFile == INVALID_HANDLE_VALUE && noBuf) {
        noBuf = false;
        hFile = CreateFileW(cfg.filePath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL,
                            OPEN_ALWAYS, FILE_FLAG_RANDOM_ACCESS, NULL);
        AppendLog(cfg.stateDir, L"[worker] NO_BUFFERING unavailable, fallback to buffered mode");
    }
    if (hFile == INVALID_HANDLE_VALUE) {
        AppendLog(cfg.stateDir, L"[worker] CreateFile failed, error=" + FormatInt(GetLastError()));
        DeleteFileW((cfg.stateDir + L"\\" + PID_FILE).c_str());
        SetThreadExecutionState(ES_CONTINUOUS);
        return 3;
    }

    // --- set the logical length of the working set ---
    {
        LARGE_INTEGER cur;
        cur.QuadPart = 0;
        GetFileSizeEx(hFile, &cur);
        if (cur.QuadPart != (LONGLONG)cfg.workingSetBytes) {
            LARGE_INTEGER target;
            target.QuadPart = (LONGLONG)cfg.workingSetBytes;
            SetFilePointerEx(hFile, target, NULL, FILE_BEGIN);
            if (!SetEndOfFile(hFile)) {
                AppendLog(cfg.stateDir, L"[worker] SetEndOfFile failed, error=" +
                          FormatInt(GetLastError()));
            }
            SetFilePointerEx(hFile, target, NULL, FILE_BEGIN);
            FlushFileBuffers(hFile);
        }
    }

    // --- aligned payload buffer with pseudo random content ---
    void* raw = _aligned_malloc((size_t)cfg.blockBytes, 4096);
    if (!raw) {
        CloseHandle(hFile);
        DeleteFileW((cfg.stateDir + L"\\" + PID_FILE).c_str());
        SetThreadExecutionState(ES_CONTINUOUS);
        return 4;
    }
    memset(raw, 0, (size_t)cfg.blockBytes);
    {
        std::mt19937_64 rng((uint64_t)GetCurrentProcessId() * 1099511628211ull + 12345ull);
        uint64_t* p = (uint64_t*)raw;
        size_t n = (size_t)(cfg.blockBytes / 8);
        for (size_t i = 0; i < n; i++) p[i] = rng();
    }

    // --- stop handles ---
    HANDLE stopHandles[3];
    DWORD  stopCount = 0;
    if (hStopA) stopHandles[stopCount++] = hStopA;
    if (hStopB) stopHandles[stopCount++] = hStopB;
    if (hStopC) stopHandles[stopCount++] = hStopC;

    // --- main loop ---
    SYSTEMTIME tStartLocal, tEndLocal;
    GetLocalTime(&tStartLocal);

    QpcClock clock;
    clock.Init();
    LARGE_INTEGER base;
    clock.Now(base);

    Stats stats;
    stats.allocatedBytes = GetFileAllocatedBytes(hFile);
    Segment seg;
    seg.startSec = 0; seg.writes = 0; seg.bytes = 0;
    seg.sumWriteUs = 0; seg.sumSyncUs = 0; seg.maxSyncUs = 0;

    uint64_t blocks = cfg.workingSetBytes / cfg.blockBytes;
    if (blocks == 0) blocks = 1;

    std::mt19937_64 rng((uint64_t)GetTickCount64() ^ (uint64_t)(uintptr_t)raw);

    double nextSegmentSec = (double)cfg.segmentSec;
    double nextReportSec  = (double)cfg.reportIntervalSec;

    const wchar_t* reason = REASON_STOPPED;
    uint32_t consecutiveErrors = 0;

    while (true) {
        if (stopCount > 0) {
            DWORD w = WaitForMultipleObjects(stopCount, stopHandles, FALSE, 0);
            if (w >= WAIT_OBJECT_0 && w < WAIT_OBJECT_0 + stopCount) {
                reason = REASON_STOPPED;
                break;
            }
        }

        uint64_t blockIndex = rng() % blocks;
        OVERLAPPED ov;
        ZeroMemory(&ov, sizeof(ov));
        ov.Offset     = (DWORD)((blockIndex * cfg.blockBytes) & 0xFFFFFFFFull);
        ov.OffsetHigh = (DWORD)((blockIndex * cfg.blockBytes) >> 32);

        LARGE_INTEGER t0, t1, t2;
        clock.Now(t0);
        DWORD written = 0;
        BOOL okWrite = WriteFile(hFile, raw, (DWORD)cfg.blockBytes, &written, &ov);
        clock.Now(t1);
        BOOL okSync = FlushFileBuffers(hFile);
        clock.Now(t2);

        if (!okWrite || written != (DWORD)cfg.blockBytes) {
            stats.writeErrors++;
            consecutiveErrors++;
        } else {
            consecutiveErrors = 0;
            double wUs = clock.ElapsedUs(t0, t1);
            stats.writes++;
            stats.bytes += written;
            stats.AddWrite(wUs);
            seg.writes++;
            seg.bytes += written;
            seg.sumWriteUs += wUs;
        }

        if (!okSync) {
            stats.syncErrors++;
        } else {
            double sUs = clock.ElapsedUs(t1, t2);
            stats.AddSync(sUs);
            seg.sumSyncUs += sUs;
            if (sUs > seg.maxSyncUs) seg.maxSyncUs = sUs;
        }

        if (consecutiveErrors >= 20) {
            reason = REASON_ERROR;
            break;
        }

        // sample the real on-disk usage (it can only grow up to the fixed working set)
        if (stats.writes % 512 == 0) {
            uint64_t allocated = GetFileAllocatedBytes(hFile);
            if (allocated > 0) stats.allocatedBytes = allocated;
        }

        LARGE_INTEGER now;
        clock.Now(now);
        double elapsed = clock.ElapsedSec(base, now);

        if (elapsed >= nextSegmentSec) {
            stats.segments.push_back(seg);
            seg.startSec = nextSegmentSec;
            seg.writes = 0; seg.bytes = 0;
            seg.sumWriteUs = 0; seg.sumSyncUs = 0; seg.maxSyncUs = 0;
            nextSegmentSec += (double)cfg.segmentSec;
        }

        if (elapsed >= nextReportSec) {
            GetLocalTime(&tEndLocal);
            std::wstring out;
            if (WriteReport(cfg, stats, tStartLocal, tEndLocal, elapsed,
                            REASON_PERIODIC, noBuf, &out)) {
                AppendLog(cfg.stateDir, L"[worker] periodic report written: " + out);
            }
            nextReportSec += (double)cfg.reportIntervalSec;
            stats.segments.clear();   // keep memory bounded across periods
        }
    }

    // --- final report ---
    LARGE_INTEGER endTick;
    clock.Now(endTick);
    double totalElapsed = clock.ElapsedSec(base, endTick);
    stats.segments.push_back(seg);
    GetLocalTime(&tEndLocal);
    {
        std::wstring out;
        if (WriteReport(cfg, stats, tStartLocal, tEndLocal, totalElapsed, reason, noBuf, &out)) {
            AppendLog(cfg.stateDir, L"[worker] final report written: " + out);
        }
    }

    _aligned_free(raw);
    CloseHandle(hFile);

    // --- release the stress file so disk usage goes back to ~0 ---
    if (cfg.deleteOnExit) {
        if (DeleteFileW(cfg.filePath.c_str())) {
            AppendLog(cfg.stateDir, L"[worker] stress file deleted: " + cfg.filePath);
        } else {
            AppendLog(cfg.stateDir, L"[worker] failed to delete stress file, error=" +
                      FormatInt(GetLastError()));
        }
    }

    DeleteFileW((cfg.stateDir + L"\\" + PID_FILE).c_str());
    SetThreadExecutionState(ES_CONTINUOUS);
    return 0;
}
