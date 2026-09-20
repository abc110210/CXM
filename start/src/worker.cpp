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

// debug.log line: [OK]/[FAIL] + step name + detail
static void Dbg(const std::wstring& dir, const wchar_t* step, bool ok, const std::wstring& detail) {
    AppendDebugLog(dir, std::wstring(ok ? L"[OK]   " : L"[FAIL] ") + step + L" | " + detail);
}

int RunWorker(const Config& cfgIn, HANDLE hStopA, HANDLE hStopB, HANDLE hStopC) {
    Config cfg = cfgIn;

    Dbg(cfg.stateDir, L"启动", true, L"RunWorker 进入，PID=" + FormatInt(GetCurrentProcessId()));

    bool dirState = EnsureDir(cfg.stateDir);
    bool dirReport = EnsureDir(cfg.reportDir);
    Dbg(cfg.stateDir, L"状态目录", dirState, cfg.stateDir + (dirState ? L"" : L" 创建失败"));
    Dbg(cfg.stateDir, L"报告目录", dirReport, cfg.reportDir + (dirReport ? L"" : L" 创建失败"));
    if (!dirState)  return 2;
    if (!dirReport) return 2;

    // --- pid file (used by the stop console to wait for a clean exit) ---
    {
        wchar_t pid[32];
        swprintf(pid, 32, L"%lu", GetCurrentProcessId());
        bool pidOk = WriteTextFileUtf8(cfg.stateDir + L"\\" + PID_FILE, std::wstring(pid));
        Dbg(cfg.stateDir, L"PID 文件", pidOk, std::wstring(L"pid=") + pid);
    }

    // --- keep the system awake during the stress run ---
    {
        DWORD prev = SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
        Dbg(cfg.stateDir, L"防休眠", prev != 0, L"SetThreadExecutionState 返回值非零即成功");
    }

    // --- never eat more than half of what is left on the volume ---
    {
        ULARGE_INTEGER freeBytes;
        std::wstring root = (cfg.filePath.size() >= 2 && cfg.filePath[1] == L':')
                                ? cfg.filePath.substr(0, 2) + L"\\" : L"C:\\";
        if (GetDiskFreeSpaceExW(root.c_str(), &freeBytes, NULL, NULL)) {
            uint64_t budget = (uint64_t)((double)freeBytes.QuadPart * 0.5);
            budget = (budget / cfg.blockBytes) * cfg.blockBytes;
            bool clamped = (budget < cfg.workingSetBytes);
            if (clamped) cfg.workingSetBytes = budget;
            Dbg(cfg.stateDir, L"可用空间", budget >= cfg.blockBytes,
                L"剩余=" + FormatBytes((uint64_t)freeBytes.QuadPart) +
                L"，覆盖区=" + FormatBytes(cfg.workingSetBytes) +
                (clamped ? L"（已被下调）" : L""));
        }
    }

    // --- sector size check for unbuffered I/O ---
    bool noBuf = cfg.noBuffering;
    {
        std::wstring root = (cfg.filePath.size() >= 2 && cfg.filePath[1] == L':')
                                ? cfg.filePath.substr(0, 2) + L"\\" : L"C:\\";
        DWORD spc = 0, bps = 0, nfc = 0, tnc = 0;
        bool secOk = (GetDiskFreeSpaceW(root.c_str(), &spc, &bps, &nfc, &tnc) == TRUE);
        if (!secOk || bps == 0) bps = 4096;
        if ((cfg.blockBytes % bps) != 0) noBuf = false;
        Dbg(cfg.stateDir, L"扇区大小", secOk,
            L"bytesPerSector=" + FormatInt(bps) +
            L"，块=" + FormatInt(cfg.blockBytes) +
            L" -> " + (noBuf ? L"NO_BUFFERING" : L"回退缓存模式"));
    }

    // --- the stress file lives in TMP\DiskStress, make sure that folder exists ---
    {
        std::wstring fileDir = cfg.filePath;
        size_t slash = fileDir.find_last_of(L"\\/");
        if (slash != std::wstring::npos) fileDir = fileDir.substr(0, slash);
        bool fileDirOk = EnsureDir(fileDir);
        Dbg(cfg.stateDir, L"压力文件目录", fileDirOk,
            fileDir + (fileDirOk ? L"" : L" 创建失败"));
        if (!fileDirOk) {
            cfg.filePath = cfg.stateDir + L"\\" + kStressFileName;
            Dbg(cfg.stateDir, L"压力文件改路径", true, L"回退到 " + cfg.filePath);
        }
    }

    // --- open / create the stress file ---
    DWORD flags = FILE_FLAG_RANDOM_ACCESS;
    if (noBuf) flags |= FILE_FLAG_NO_BUFFERING;
    HANDLE hFile = CreateFileW(cfg.filePath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL,
                               OPEN_ALWAYS, flags, NULL);
    Dbg(cfg.stateDir, L"打开压力文件", hFile != INVALID_HANDLE_VALUE,
        cfg.filePath + (hFile != INVALID_HANDLE_VALUE ? L"" :
                        (L" 失败 err=" + FormatInt(GetLastError()))));
    if (hFile == INVALID_HANDLE_VALUE && noBuf) {
        noBuf = false;
        hFile = CreateFileW(cfg.filePath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL,
                            OPEN_ALWAYS, FILE_FLAG_RANDOM_ACCESS, NULL);
        Dbg(cfg.stateDir, L"回退打开(带缓存)", hFile != INVALID_HANDLE_VALUE, cfg.filePath);
        AppendLog(cfg.stateDir, L"[worker] NO_BUFFERING unavailable, fallback to buffered mode");
    }
    if (hFile == INVALID_HANDLE_VALUE) {
        AppendLog(cfg.stateDir, L"[worker] CreateFile failed, error=" + FormatInt(GetLastError()));
        DeleteFileW((cfg.stateDir + L"\\" + PID_FILE).c_str());
        SetThreadExecutionState(ES_CONTINUOUS);
        Dbg(cfg.stateDir, L"退出", false, L"压力文件无法打开，返回码=3");
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
            BOOL sizeOk = SetEndOfFile(hFile);
            SetFilePointerEx(hFile, target, NULL, FILE_BEGIN);
            FlushFileBuffers(hFile);
            Dbg(cfg.stateDir, L"设置文件长度", sizeOk == TRUE,
                L"目标=" + FormatBytes(cfg.workingSetBytes) +
                (sizeOk ? L"" : (L" 失败 err=" + FormatInt(GetLastError()))));
        } else {
            Dbg(cfg.stateDir, L"设置文件长度", true,
                L"已是目标大小 " + FormatBytes(cfg.workingSetBytes));
        }
        LARGE_INTEGER nowSize;
        nowSize.QuadPart = 0;
        GetFileSizeEx(hFile, &nowSize);
        Dbg(cfg.stateDir, L"文件实际长度", nowSize.QuadPart == (LONGLONG)cfg.workingSetBytes,
            FormatBytes((uint64_t)nowSize.QuadPart));
    }

    // --- aligned payload buffer with pseudo random content ---
    void* raw = _aligned_malloc((size_t)cfg.blockBytes, 4096);
    Dbg(cfg.stateDir, L"分配写缓冲", raw != NULL, FormatBytes(cfg.blockBytes) + L" 对齐 4096");
    if (!raw) {
        CloseHandle(hFile);
        DeleteFileW((cfg.stateDir + L"\\" + PID_FILE).c_str());
        SetThreadExecutionState(ES_CONTINUOUS);
        Dbg(cfg.stateDir, L"退出", false, L"缓冲区分配失败，返回码=4");
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
    bool firstWriteLogged = false;

    Dbg(cfg.stateDir, L"进入写入循环", true,
        L"覆盖区=" + FormatBytes(cfg.workingSetBytes) +
        L"，块数=" + FormatInt(blocks) +
        L"，缓存模式=" + (noBuf ? L"NO_BUFFERING" : L"buffered") +
        L"，报告周期=" + FormatInt(cfg.reportIntervalSec / 60) + L" 分钟");

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

        if (!firstWriteLogged) {
            firstWriteLogged = true;
            Dbg(cfg.stateDir, L"首次写入", okWrite && written == (DWORD)cfg.blockBytes && okSync,
                L"WriteFile=" + std::wstring(okWrite ? L"OK" : L"FAIL") +
                L"，写入字节=" + FormatInt(written) +
                L"，FlushFileBuffers=" + std::wstring(okSync ? L"OK" : L"FAIL") +
                L"，sync=" + FormatDouble(clock.ElapsedUs(t1, t2) / 1000.0, 3) + L" ms" +
                L"，偏移块号=" + FormatInt(blockIndex));
        }

        // sample the real on-disk usage (it can only grow up to the fixed working set)
        if (stats.writes % 512 == 0) {
            uint64_t allocated = GetFileAllocatedBytes(hFile);
            if (allocated > 0) stats.allocatedBytes = allocated;
        }

        if (stats.writes % 10000 == 0 && stats.writes > 0) {
            Dbg(cfg.stateDir, L"心跳", true,
                L"累计写入=" + FormatInt(stats.writes) +
                L"，累计字节=" + FormatBytes(stats.bytes) +
                L"，实际占用=" + FormatBytes(stats.allocatedBytes) +
                L"，写错误=" + FormatInt(stats.writeErrors) +
                L"，同步错误=" + FormatInt(stats.syncErrors));
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
            bool repOk = WriteReport(cfg, stats, tStartLocal, tEndLocal, elapsed,
                                     REASON_PERIODIC, noBuf, &out);
            Dbg(cfg.stateDir, L"周期报告", repOk, repOk ? out : L"写入失败");
            if (repOk) AppendLog(cfg.stateDir, L"[worker] periodic report written: " + out);
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
    Dbg(cfg.stateDir, L"停止原因", true, reason);
    {
        std::wstring out;
        bool repOk = WriteReport(cfg, stats, tStartLocal, tEndLocal, totalElapsed, reason, noBuf, &out);
        Dbg(cfg.stateDir, L"最终报告", repOk, repOk ? out : L"写入失败");
        if (repOk) AppendLog(cfg.stateDir, L"[worker] final report written: " + out);
    }

    _aligned_free(raw);
    CloseHandle(hFile);
    Dbg(cfg.stateDir, L"关闭压力文件", true, cfg.filePath);

    // --- release the stress file so disk usage goes back to ~0 ---
    if (cfg.deleteOnExit) {
        bool delOk = DeleteFileW(cfg.filePath.c_str()) == TRUE;
        Dbg(cfg.stateDir, L"删除压力文件", delOk,
            cfg.filePath + (delOk ? L"" : (L" 失败 err=" + FormatInt(GetLastError()))));
        if (delOk) AppendLog(cfg.stateDir, L"[worker] stress file deleted: " + cfg.filePath);
        else AppendLog(cfg.stateDir, L"[worker] failed to delete stress file, error=" +
                       FormatInt(GetLastError()));
    } else {
        Dbg(cfg.stateDir, L"删除压力文件", true, L"配置为保留，跳过");
    }

    DeleteFileW((cfg.stateDir + L"\\" + PID_FILE).c_str());
    SetThreadExecutionState(ES_CONTINUOUS);
    Dbg(cfg.stateDir, L"退出", true, L"RunWorker 正常结束，返回码=0");
    return 0;
}
