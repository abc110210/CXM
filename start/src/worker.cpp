// 高 IOPS 异步队列写入引擎（多线程 × 队列深度 + 在线模式）
//   - N 个线程，每线程维持 QD 个未完成的 overlapped 写
//   - 启动时预填充整个覆盖区，之后所有写入都是"覆盖已有数据"
//   - 周期性 FlushFileBuffers（独立线程），退出前再强制一次
//   - 服务器可实时下发配置：IOPS 限制 / 线程数 / 队列深度 / 块大小
#include "worker.h"
#include "report.h"
#include "net.h"

#include <cstdio>
#include <cstring>
#include <malloc.h>
#include <random>
#include <vector>

#pragma comment(lib, "kernel32.lib")

static const wchar_t* REASON_PERIODIC = L"周期报告（每 5 小时一份）";
static const wchar_t* REASON_STOPPED  = L"收到停止指令（DiskStress 控制台）";
static const wchar_t* REASON_ERROR    = L"连续 I/O 错误，程序自我保护退出";

static LARGE_INTEGER g_freq;

static uint64_t NowTick() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint64_t)t.QuadPart;
}

static double TickDeltaUs(uint64_t a, uint64_t b) {
    return (double)((LONGLONG)b - (LONGLONG)a) * 1000000.0 / (double)g_freq.QuadPart;
}

static void Dbg(const std::wstring& dir, const wchar_t* step, bool ok, const std::wstring& detail) {
    AppendDebugLog(dir, std::wstring(ok ? L"[OK]   " : L"[FAIL] ") + step + L" | " + detail);
}

// ---------------- 在线模式运行时状态 ----------------
// g_rt 定义在 shared.cpp；这里只放实时统计与限速器
static CRITICAL_SECTION g_liveLock;
static bool             g_liveInit = false;
static LiveStats        g_live;
static uint64_t         g_livePrevWrites = 0;
static uint64_t         g_livePrevBytes  = 0;
static uint64_t         g_livePrevTick   = 0;

static void LiveInit() {
    if (!g_liveInit) { InitializeCriticalSection(&g_liveLock); g_liveInit = true; }
}

// 令牌桶限速：rate=0 表示不限速
struct RateLimiter {
    CRITICAL_SECTION lock;
    bool     init;
    double   tokens;
    uint64_t lastTick;
    uint32_t rate;
    RateLimiter() : init(false), tokens(0), lastTick(0), rate(0) {}
};
static RateLimiter g_limiter;

static void LimiterSetRate(uint32_t rate) {
    if (!g_limiter.init) { InitializeCriticalSection(&g_limiter.lock); g_limiter.init = true; }
    EnterCriticalSection(&g_limiter.lock);
    if (g_limiter.rate != rate) {
        g_limiter.rate  = rate;
        g_limiter.tokens = 0;
    }
    LeaveCriticalSection(&g_limiter.lock);
}

static bool LimiterTake() {
    if (!g_limiter.init || g_limiter.rate == 0) return true;
    EnterCriticalSection(&g_limiter.lock);
    uint64_t now = NowTick();
    double sec = TickDeltaUs(g_limiter.lastTick, now) / 1000000.0;
    g_limiter.lastTick = now;
    g_limiter.tokens += sec * (double)g_limiter.rate;
    double cap = (double)g_limiter.rate;   // 最多积攒 1 秒的突发
    if (g_limiter.tokens > cap) g_limiter.tokens = cap;
    bool ok = (g_limiter.tokens >= 1.0);
    if (ok) g_limiter.tokens -= 1.0;
    LeaveCriticalSection(&g_limiter.lock);
    return ok;
}

static void LiveUpdate(const Stats& snap, uint64_t uptimeSec) {
    if (!g_liveInit) { InitializeCriticalSection(&g_liveLock); g_liveInit = true; }
    double dt = TickDeltaUs(g_livePrevTick, NowTick()) / 1000000.0;
    double iops = 0, mbps = 0;
    if (dt > 0.3) {
        iops = (double)(snap.writes - g_livePrevWrites) / dt;
        mbps = (double)((LONGLONG)snap.bytes - (LONGLONG)g_livePrevBytes) / dt / (1024.0 * 1024.0);
        if (mbps < 0) mbps = 0;
        g_livePrevWrites = snap.writes;
        g_livePrevBytes  = snap.bytes;
        g_livePrevTick   = NowTick();
    }
    EnterCriticalSection(&g_liveLock);
    g_live.writes    = snap.writes;
    g_live.errors    = snap.writeErrors + snap.syncErrors;
    g_live.iops      = iops;
    g_live.mbps      = mbps;
    g_live.uptimeSec = uptimeSec;
    g_live.cfg       = RtGet(&g_rt);
    LeaveCriticalSection(&g_liveLock);
}

static void LiveStatsProvider(LiveStats* out) {
    if (!g_liveInit) { InitializeCriticalSection(&g_liveLock); g_liveInit = true; }
    EnterCriticalSection(&g_liveLock);
    *out = g_live;
    LeaveCriticalSection(&g_liveLock);
    out->cfg = RtGet(&g_rt);
}

// --------------------------------------------------------------- contexts

struct IoStats {
    uint64_t    completed;
    uint64_t    bytes;
    uint64_t    errors;
    double      sumUs;
    double      minUs;
    double      maxUs;
    LatencyHist hist;
    IoStats() : completed(0), bytes(0), errors(0), sumUs(0), minUs(1e18), maxUs(0) {}
};

struct ThreadCtx {
    HANDLE                  hFile;
    HANDLE                  hStop;
    uint64_t                blocks;
    uint64_t                blockBytes;
    uint32_t                qd;
    std::vector<HANDLE>     events;
    std::vector<OVERLAPPED> ovs;
    std::vector<uint64_t>   submitTick;
    std::vector<char*>      bufs;
    char*                   arena;
    IoStats                 st;
    std::mt19937_64         rng;
    bool                    fatal;
    ThreadCtx() : hFile(NULL), hStop(NULL), blocks(0), blockBytes(0), qd(1),
                  arena(NULL), rng(0), fatal(false) {}
};

struct SyncCtx {
    HANDLE      hFile;
    HANDLE      hStop;
    uint32_t    intervalSec;
    uint64_t    count;
    uint64_t    failures;
    double      sumUs;
    double      maxUs;
    LatencyHist hist;
    SyncCtx() : hFile(NULL), hStop(NULL), intervalSec(10), count(0), failures(0),
                sumUs(0), maxUs(0) {}
};

// ------------------------------------------------------------ io thread

static bool SubmitIo(ThreadCtx* c, uint32_t slot) {
    while (!LimiterTake()) Sleep(1);
    for (int retry = 0; retry < 5; retry++) {
        ZeroMemory(&c->ovs[slot], sizeof(OVERLAPPED));
        c->ovs[slot].hEvent = c->events[slot];

        uint64_t idx = c->rng() % c->blocks;
        uint64_t off = idx * c->blockBytes;
        c->ovs[slot].Offset     = (DWORD)(off & 0xFFFFFFFFull);
        c->ovs[slot].OffsetHigh = (DWORD)(off >> 32);

        c->submitTick[slot] = NowTick();
        BOOL ok = WriteFile(c->hFile, c->bufs[slot], (DWORD)c->blockBytes, NULL, &c->ovs[slot]);
        if (ok || GetLastError() == ERROR_IO_PENDING) return true;

        c->st.errors++;
        Sleep(10);
    }
    return false;
}

static void CompleteIo(ThreadCtx* c, uint32_t slot) {
    double us = TickDeltaUs(c->submitTick[slot], NowTick());
    c->st.completed++;
    c->st.bytes += c->blockBytes;
    c->st.sumUs += us;
    if (us < c->st.minUs) c->st.minUs = us;
    if (us > c->st.maxUs) c->st.maxUs = us;
    c->st.hist.Add(us);
}

static DWORD WINAPI IoThread(LPVOID p) {
    ThreadCtx* c = (ThreadCtx*)p;
    const DWORD qd = (DWORD)c->qd;

    for (DWORD i = 0; i < qd; i++) {
        if (!SubmitIo(c, i)) { c->fatal = true; return 1; }
    }

    std::vector<HANDLE> waits((size_t)qd + 1);
    while (true) {
        for (DWORD i = 0; i < qd; i++) waits[i] = c->events[i];
        waits[qd] = c->hStop;

        DWORD w = WaitForMultipleObjects(qd + 1, &waits[0], FALSE, INFINITE);
        if (w == WAIT_OBJECT_0 + qd) break;                 // stop requested
        if (w == WAIT_TIMEOUT) continue;
        if (w >= WAIT_OBJECT_0 && w < WAIT_OBJECT_0 + qd) {
            DWORD slot = w - WAIT_OBJECT_0;
            CompleteIo(c, slot);
            if (!SubmitIo(c, slot)) { c->fatal = true; break; }
        }
    }

    // drain：StopPool 已 CancelIoEx，未完成的会立即以取消状态置位事件
    for (DWORD i = 0; i < qd; i++) {
        if (WaitForSingleObject(c->events[i], 2000) == WAIT_OBJECT_0) CompleteIo(c, i);
    }
    return 0;
}

static DWORD WINAPI SyncThread(LPVOID p) {
    SyncCtx* c = (SyncCtx*)p;
    while (WaitForSingleObject(c->hStop, c->intervalSec * 1000) == WAIT_TIMEOUT) {
        uint64_t t0 = NowTick();
        BOOL ok = FlushFileBuffers(c->hFile);
        double us = TickDeltaUs(t0, NowTick());
        if (ok) {
            c->count++;
            c->sumUs += us;
            if (us > c->maxUs) c->maxUs = us;
            c->hist.Add(us);
        } else {
            c->failures++;
        }
    }
    return 0;
}

// ------------------------------------------------------------- prefill

static bool PrefillFile(HANDLE hFile, const Config& cfg, double* seconds, uint64_t* written) {
    const uint64_t chunk = 1024 * 1024; // 1 MiB
    void* buf = _aligned_malloc((size_t)chunk, 4096);
    if (!buf) return false;
    memset(buf, 0xA5, (size_t)chunk);

    HANDLE ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ev) { _aligned_free(buf); return false; }

    uint64_t t0 = NowTick();
    uint64_t off = 0;
    bool ok = true;
    while (off < cfg.workingSetBytes) {
        uint64_t n = chunk;
        if (off + n > cfg.workingSetBytes) n = cfg.workingSetBytes - off;

        OVERLAPPED ov;
        ZeroMemory(&ov, sizeof(ov));
        ov.hEvent     = ev;
        ov.Offset     = (DWORD)(off & 0xFFFFFFFFull);
        ov.OffsetHigh = (DWORD)(off >> 32);

        ResetEvent(ev);
        BOOL r = WriteFile(hFile, buf, (DWORD)n, NULL, &ov);
        if (!r && GetLastError() == ERROR_IO_PENDING) {
            r = (WaitForSingleObject(ev, 60000) == WAIT_OBJECT_0);
        }
        if (!r) { ok = false; break; }
        off += n;
    }
    uint64_t t1 = NowTick();
    FlushFileBuffers(hFile);

    *seconds = TickDeltaUs(t0, t1) / 1000000.0;
    *written = off;
    CloseHandle(ev);
    _aligned_free(buf);
    return ok;
}

// ------------------------------------------------------------ snapshot

static void Snapshot(const std::vector<ThreadCtx*>& tcs, const SyncCtx& sc,
                     const Stats& base, Stats& out) {
    out = base;
    for (size_t i = 0; i < tcs.size(); i++) {
        const IoStats& s = tcs[i]->st;
        out.writes      += s.completed;
        out.bytes       += s.bytes;
        out.writeErrors += s.errors;
        out.sumWriteUs  += s.sumUs;
        if (s.minUs < out.minWriteUs) out.minWriteUs = s.minUs;
        if (s.maxUs > out.maxWriteUs) out.maxWriteUs = s.maxUs;
        for (size_t b = 0; b < out.writeHist.buckets.size(); b++) {
            out.writeHist.buckets[b] += s.hist.buckets[b];
        }
        out.writeHist.total += s.hist.total;
        out.writeHist.over  += s.hist.over;
    }
    out.syncCount  += sc.count;
    out.syncErrors += sc.failures;
    out.sumSyncUs  += sc.sumUs;
    if (sc.maxUs > out.maxSyncUs) out.maxSyncUs = sc.maxUs;
    for (size_t b = 0; b < out.syncHist.buckets.size(); b++) {
        out.syncHist.buckets[b] += sc.hist.buckets[b];
    }
    out.syncHist.total += sc.hist.total;
    out.syncHist.over  += sc.hist.over;
}

// 把当前一代线程的统计累加进 base，然后清零（换配置重建线程池时调用）
static void FoldGen(Stats& base, std::vector<ThreadCtx*>& tcs, SyncCtx& sc) {
    for (size_t i = 0; i < tcs.size(); i++) {
        IoStats& s = tcs[i]->st;   // 非 const：折叠进 base 后要把当代计数清零
        base.writes      += s.completed;
        base.bytes       += s.bytes;
        base.writeErrors += s.errors;
        base.sumWriteUs  += s.sumUs;
        if (s.minUs < base.minWriteUs) base.minWriteUs = s.minUs;
        if (s.maxUs > base.maxWriteUs) base.maxWriteUs = s.maxUs;
        for (size_t b = 0; b < base.writeHist.buckets.size(); b++) {
            base.writeHist.buckets[b] += s.hist.buckets[b];
        }
        base.writeHist.total += s.hist.total;
        base.writeHist.over  += s.hist.over;
        s.completed = 0; s.bytes = 0; s.errors = 0; s.sumUs = 0;
        s.minUs = 1e18; s.maxUs = 0; s.hist = LatencyHist();
    }
    base.syncCount  += sc.count;
    base.syncErrors += sc.failures;
    base.sumSyncUs  += sc.sumUs;
    if (sc.maxUs > base.maxSyncUs) base.maxSyncUs = sc.maxUs;
    for (size_t b = 0; b < base.syncHist.buckets.size(); b++) {
        base.syncHist.buckets[b] += sc.hist.buckets[b];
    }
    base.syncHist.total += sc.hist.total;
    base.syncHist.over  += sc.hist.over;
    sc.count = 0; sc.failures = 0; sc.sumUs = 0; sc.maxUs = 0; sc.hist = LatencyHist();
}

// ------------------------------------------------------------ RunWorker

int RunWorker(const Config& cfgIn, HANDLE hStopA, HANDLE hStopB, HANDLE hStopC) {
    Config cfg = cfgIn;
    QueryPerformanceFrequency(&g_freq);

    if (cfg.threads < 1) cfg.threads = 1;
    if (cfg.queueDepth < 1) cfg.queueDepth = 1;
    if (cfg.syncIntervalSec < 1) cfg.syncIntervalSec = 1;

    Dbg(cfg.stateDir, L"启动", true,
        L"RunWorker 进入，PID=" + FormatInt(GetCurrentProcessId()) +
        L"，线程=" + FormatInt(cfg.threads) +
        L"，队列深度=" + FormatInt(cfg.queueDepth) +
        L"，块=" + FormatBytes(cfg.blockBytes));

    bool dirState  = EnsureDir(cfg.stateDir);
    bool dirReport = EnsureDir(cfg.reportDir);
    Dbg(cfg.stateDir, L"状态目录", dirState, cfg.stateDir + (dirState ? L"" : L" 创建失败"));
    Dbg(cfg.stateDir, L"报告目录", dirReport, cfg.reportDir + (dirReport ? L"" : L" 创建失败"));
    if (!dirState)  return 2;
    if (!dirReport) return 2;

    {
        wchar_t pid[32];
        swprintf(pid, 32, L"%lu", GetCurrentProcessId());
        bool pidOk = WriteTextFileUtf8(cfg.stateDir + L"\\" + PID_FILE, std::wstring(pid));
        Dbg(cfg.stateDir, L"PID 文件", pidOk, std::wstring(L"pid=") + pid);
    }

    {
        DWORD prev = SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
        Dbg(cfg.stateDir, L"防休眠", prev != 0, L"SetThreadExecutionState 返回值非零即成功");
    }

    // --- 卷剩余空间护栏：最多吃掉剩余空间的一半 ---
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

    // --- 扇区大小检查（决定能否 NO_BUFFERING）---
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

    // --- 清理上次非正常退出残留的压力文件 ---
    if (cfg.deleteOnExit &&
        GetFileAttributesW(cfg.filePath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        BOOL gone = DeleteFileW(cfg.filePath.c_str());
        Dbg(cfg.stateDir, L"清理残留压力文件", gone == TRUE,
            cfg.filePath + (gone ? L"" : (L" 失败 err=" + FormatInt(GetLastError()))));
    }

    // --- 确保压力文件父目录存在 ---
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

    // --- 打开压力文件（overlapped 是队列深度的前提）---
    DWORD flags = FILE_FLAG_RANDOM_ACCESS | FILE_FLAG_OVERLAPPED;
    if (noBuf) flags |= FILE_FLAG_NO_BUFFERING;
    HANDLE hFile = CreateFileW(cfg.filePath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL,
                               OPEN_ALWAYS, flags, NULL);
    Dbg(cfg.stateDir, L"打开压力文件", hFile != INVALID_HANDLE_VALUE,
        cfg.filePath + (hFile != INVALID_HANDLE_VALUE ? L"" :
                        (L" 失败 err=" + FormatInt(GetLastError()))));
    if (hFile == INVALID_HANDLE_VALUE && noBuf) {
        noBuf = false;
        hFile = CreateFileW(cfg.filePath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL,
                            OPEN_ALWAYS, FILE_FLAG_RANDOM_ACCESS | FILE_FLAG_OVERLAPPED, NULL);
        Dbg(cfg.stateDir, L"回退打开(带缓存)", hFile != INVALID_HANDLE_VALUE, cfg.filePath);
    }
    if (hFile == INVALID_HANDLE_VALUE) {
        DeleteFileW((cfg.stateDir + L"\\" + PID_FILE).c_str());
        SetThreadExecutionState(ES_CONTINUOUS);
        Dbg(cfg.stateDir, L"退出", false, L"压力文件无法打开，返回码=3");
        return 3;
    }

    // --- 固定覆盖区长度 ---
    {
        LARGE_INTEGER cur;
        cur.QuadPart = 0;
        GetFileSizeEx(hFile, &cur);
        LARGE_INTEGER target;
        target.QuadPart = (LONGLONG)cfg.workingSetBytes;
        BOOL sizeOk = TRUE;
        if (cur.QuadPart != target.QuadPart) {
            SetFilePointerEx(hFile, target, NULL, FILE_BEGIN);
            sizeOk = SetEndOfFile(hFile);
            SetFilePointerEx(hFile, target, NULL, FILE_BEGIN);
            FlushFileBuffers(hFile);
        }
        Dbg(cfg.stateDir, L"设置文件长度", sizeOk == TRUE,
            L"目标=" + FormatBytes(cfg.workingSetBytes) +
            (sizeOk ? L"" : (L" 失败 err=" + FormatInt(GetLastError()))));
    }

    // --- 预填充：之后所有写入都是覆盖已有数据 ---
    Stats accum;      // 跨代累计基线（重建线程池时不清零）
    if (cfg.prefill) {
        double sec = 0;
        uint64_t written = 0;
        bool ok = PrefillFile(hFile, cfg, &sec, &written);
        accum.prefillDone = ok;
        accum.prefillSec  = sec;
        Dbg(cfg.stateDir, L"预填充覆盖区", ok,
            L"写入=" + FormatBytes(written) +
            L"，耗时=" + FormatDouble(sec, 1) + L" s" +
            (sec > 0 ? (L"，吞吐=" +
                        FormatDouble((double)written / sec / (1024.0 * 1024.0), 1) + L" MiB/s")
                     : L""));
    } else {
        Dbg(cfg.stateDir, L"预填充覆盖区", true, L"配置为关闭，跳过");
    }
    accum.allocatedBytes = GetFileAllocatedBytes(hFile);

    // --- 在线模式运行时配置 ---
    {
        CfgVals init;
        init.threads    = cfg.threads;
        init.queueDepth = cfg.queueDepth;
        init.blockBytes = (uint32_t)cfg.blockBytes;
        init.iopsLimit  = 0;
        init.version    = 1;
        RtInit(&g_rt, init);
        LiveInit();
        LimiterSetRate(0);
    }

    // --- 线程池（可整代重建）---
    std::vector<ThreadCtx*> tcs;
    std::vector<HANDLE>     ths;
    HANDLE hStopInternal = NULL;
    HANDLE syncTh        = NULL;
    SyncCtx sc;
    bool poolReady = false;
    CfgVals applied;
    applied.threads    = cfg.threads;
    applied.queueDepth = cfg.queueDepth;
    applied.blockBytes = (uint32_t)cfg.blockBytes;
    applied.iopsLimit  = 0;
    applied.version    = 1;

    uint64_t blocks = cfg.workingSetBytes / cfg.blockBytes;
    if (blocks == 0) blocks = 1;

    auto StopPool = [&](void) {
        if (hStopInternal) SetEvent(hStopInternal);
        if (hFile != INVALID_HANDLE_VALUE) CancelIoEx(hFile, NULL);  // 取消在飞 I/O，缓冲区才能安全释放
        for (size_t i = 0; i < ths.size(); i++) WaitForSingleObject(ths[i], 60000);
        for (size_t i = 0; i < ths.size(); i++) CloseHandle(ths[i]);
        ths.clear();
        if (syncTh) { WaitForSingleObject(syncTh, 30000); CloseHandle(syncTh); syncTh = NULL; }
    };
    auto FreePool = [&](void) {
        for (size_t i = 0; i < tcs.size(); i++) {
            for (uint32_t k = 0; k < tcs[i]->qd; k++) if (tcs[i]->events[k]) CloseHandle(tcs[i]->events[k]);
            if (tcs[i]->arena) _aligned_free(tcs[i]->arena);
            delete tcs[i];
        }
        tcs.clear();
        if (hStopInternal) { CloseHandle(hStopInternal); hStopInternal = NULL; }
    };
    auto BuildPool = [&](uint32_t nThreads, uint32_t qd, uint64_t blockBytes) -> bool {
        hStopInternal = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!hStopInternal) return false;
        uint64_t blocks2 = cfg.workingSetBytes / blockBytes;
        if (blocks2 == 0) blocks2 = 1;
        for (uint32_t t = 0; t < nThreads; t++) {
            ThreadCtx* c = new ThreadCtx();
            c->hFile      = hFile;
            c->hStop      = hStopInternal;
            c->blocks     = blocks2;
            c->blockBytes = blockBytes;
            c->qd         = qd;
            c->rng.seed((uint64_t)GetTickCount64() * 1099511628211ull +
                        (uint64_t)(t + 1) * 7919ull + (uint64_t)(uintptr_t)c);
            c->events.assign(qd, NULL);
            c->ovs.resize(qd);
            c->submitTick.assign(qd, 0);
            c->bufs.assign(qd, NULL);
            c->arena = (char*)_aligned_malloc((size_t)qd * (size_t)blockBytes, 4096);
            if (!c->arena) { delete c; return false; }
            memset(c->arena, 0x5A, (size_t)qd * (size_t)blockBytes);
            for (uint32_t i = 0; i < qd; i++) {
                c->bufs[i] = c->arena + (size_t)i * (size_t)blockBytes;
                c->events[i] = CreateEventW(NULL, FALSE, FALSE, NULL);   // auto reset
                if (!c->events[i]) return false;
            }
            tcs.push_back(c);
        }
        for (uint32_t t = 0; t < nThreads; t++) {
            HANDLE th = CreateThread(NULL, 0, IoThread, tcs[t], 0, NULL);
            if (!th) return false;
            ths.push_back(th);
        }
        sc.hFile       = hFile;
        sc.hStop       = hStopInternal;
        sc.intervalSec = cfg.syncIntervalSec;
        syncTh = CreateThread(NULL, 0, SyncThread, &sc, 0, NULL);
        return syncTh != NULL;
    };

    poolReady = BuildPool(cfg.threads, cfg.queueDepth, cfg.blockBytes);
    Dbg(cfg.stateDir, L"启动线程池", poolReady,
        FormatBytes((uint64_t)cfg.threads * cfg.queueDepth * cfg.blockBytes) +
        L"（" + FormatInt(cfg.threads) + L" 线程 x QD" + FormatInt(cfg.queueDepth) +
        L"，块数=" + FormatInt(blocks) + L"）");
    if (!poolReady) {
        StopPool();
        FreePool();
        CloseHandle(hFile);
        DeleteFileW((cfg.stateDir + L"\\" + PID_FILE).c_str());
        SetThreadExecutionState(ES_CONTINUOUS);
        Dbg(cfg.stateDir, L"退出", false, L"线程池启动失败，返回码=4");
        return 4;
    }

    Dbg(cfg.stateDir, L"进入写入循环", true,
        L"覆盖区=" + FormatBytes(cfg.workingSetBytes) +
        L"，块数=" + FormatInt(blocks) +
        L"，线程=" + FormatInt(cfg.threads) +
        L"，队列深度=" + FormatInt(cfg.queueDepth) +
        L"，fsync 周期=" + FormatInt(cfg.syncIntervalSec) + L" s" +
        L"，缓存模式=" + (noBuf ? L"NO_BUFFERING" : L"buffered") +
        L"，报告周期=" + FormatInt(cfg.reportIntervalSec / 60) + L" 分钟");

    // --- 监督循环 ---
    SYSTEMTIME tStartLocal, tEndLocal;
    GetLocalTime(&tStartLocal);
    uint64_t base = NowTick();

    HANDLE stopHandles[3];
    DWORD  stopCount = 0;
    if (hStopA) stopHandles[stopCount++] = hStopA;
    if (hStopB) stopHandles[stopCount++] = hStopB;
    if (hStopC) stopHandles[stopCount++] = hStopC;

    const wchar_t* reason = REASON_STOPPED;
    double nextSegmentSec = (double)cfg.segmentSec;
    double nextReportSec  = (double)cfg.reportIntervalSec;
    Stats  prevSnap;
    bool   firstSnap   = true;
    bool   allocWarned = false;

    // 在线模式：连接服务器、上报状态、接收配置下发
    HANDLE hWorkerStop = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (hWorkerStop) NetStartThread(hWorkerStop, LiveStatsProvider, cfg.stateDir);

    while (poolReady) {
        bool stopReq = false;
        if (stopCount > 0) {
            DWORD w = WaitForMultipleObjects(stopCount, stopHandles, FALSE, 500);
            if (w >= WAIT_OBJECT_0 && w < WAIT_OBJECT_0 + stopCount) stopReq = true;
        } else {
            Sleep(500);
        }

        double elapsed = TickDeltaUs(base, NowTick()) / 1000000.0;

        // 实时统计（供网络线程上报给服务器）
        {
            Stats snap;
            Snapshot(tcs, sc, accum, snap);
            LiveUpdate(snap, (uint64_t)elapsed);
        }

        // 服务器下发的配置：立刻生效
        CfgVals curCfg = RtGet(&g_rt);
        if (curCfg.version != applied.version) {
            LimiterSetRate(curCfg.iopsLimit);
            if (curCfg.threads != applied.threads || curCfg.queueDepth != applied.queueDepth ||
                curCfg.blockBytes != applied.blockBytes) {
                Dbg(cfg.stateDir, L"配置生效", true,
                    L"重建线程池：线程=" + FormatInt(curCfg.threads) +
                    L"，QD=" + FormatInt(curCfg.queueDepth) +
                    L"，块=" + FormatInt(curCfg.blockBytes) +
                    L"，IOPS限制=" + FormatInt(curCfg.iopsLimit));
                StopPool();
                FoldGen(accum, tcs, sc);
                FreePool();
                cfg.blockBytes = curCfg.blockBytes;
                poolReady = BuildPool(curCfg.threads, curCfg.queueDepth, curCfg.blockBytes);
                Dbg(cfg.stateDir, L"重建线程池", poolReady,
                    poolReady ? L"新参数已生效" : L"重建失败，压力停止");
            } else {
                Dbg(cfg.stateDir, L"配置生效", true,
                    L"IOPS 限制=" + FormatInt(curCfg.iopsLimit));
            }
            applied = curCfg;
            if (!poolReady) { reason = REASON_ERROR; break; }
        }

        if (elapsed >= nextSegmentSec || stopReq) {
            Stats curSnap;
            Snapshot(tcs, sc, accum, curSnap);
            curSnap.allocatedBytes = GetFileAllocatedBytes(hFile);
            if (curSnap.allocatedBytes == 0 && !allocWarned) {
                allocWarned = true;
                Dbg(cfg.stateDir, L"占用查询", false,
                    L"GetFileAllocatedBytes 返回 0，报告中的实际占用会显示为 0");
            }
            if (!firstSnap) {
                Segment seg;
                seg.startSec   = nextSegmentSec - (double)cfg.segmentSec;
                seg.writes     = curSnap.writes - prevSnap.writes;
                seg.bytes      = curSnap.bytes - prevSnap.bytes;
                seg.sumWriteUs = curSnap.sumWriteUs - prevSnap.sumWriteUs;
                seg.sumSyncUs  = curSnap.sumSyncUs - prevSnap.sumSyncUs;
                seg.maxSyncUs  = sc.maxUs;
                accum.segments.push_back(seg);
            }
            firstSnap = false;
            prevSnap  = curSnap;
            sc.maxUs  = 0;
            if (!stopReq) nextSegmentSec += (double)cfg.segmentSec;
        }

        if (elapsed >= nextReportSec && !stopReq) {
            Stats curSnap;
            Snapshot(tcs, sc, accum, curSnap);
            curSnap.allocatedBytes = GetFileAllocatedBytes(hFile);
            GetLocalTime(&tEndLocal);
            std::wstring out;
            bool repOk = WriteReport(cfg, curSnap, tStartLocal, tEndLocal, elapsed,
                                     REASON_PERIODIC, noBuf, &out);
            Dbg(cfg.stateDir, L"周期报告", repOk, repOk ? out : L"写入失败");
            accum.segments.clear();
            nextReportSec += (double)cfg.reportIntervalSec;
        }

        if (stopReq) { reason = REASON_STOPPED; break; }
        bool fatal = false;
        for (size_t i = 0; i < tcs.size(); i++) if (tcs[i]->fatal) fatal = true;
        if (fatal) { reason = REASON_ERROR; break; }
    }

    // --- 收尾 ---
    if (hWorkerStop) SetEvent(hWorkerStop);
    StopPool();
    FoldGen(accum, tcs, sc);
    FreePool();

    {
        uint64_t t0 = NowTick();
        BOOL fs = FlushFileBuffers(hFile);
        double us = TickDeltaUs(t0, NowTick());
        Dbg(cfg.stateDir, L"退出前 fsync", fs == TRUE,
            L"耗时=" + FormatDouble(us / 1000.0, 3) + L" ms");
        if (fs) {
            sc.count++;
            sc.sumUs += us;
            if (us > sc.maxUs) sc.maxUs = us;
            sc.hist.Add(us);
        }
    }

    // --- 最终报告 ---
    double totalElapsed = TickDeltaUs(base, NowTick()) / 1000000.0;
    Stats stats;
    Snapshot(tcs, sc, accum, stats);
    stats.allocatedBytes = GetFileAllocatedBytes(hFile);
    if (!firstSnap) {
        // 最后一小段，让短时间运行也有分段表
        Segment seg;
        seg.startSec   = nextSegmentSec - (double)cfg.segmentSec;
        seg.writes     = stats.writes - prevSnap.writes;
        seg.bytes      = stats.bytes - prevSnap.bytes;
        seg.sumWriteUs = stats.sumWriteUs - prevSnap.sumWriteUs;
        seg.sumSyncUs  = stats.sumSyncUs - prevSnap.sumSyncUs;
        seg.maxSyncUs  = sc.maxUs;
        stats.segments.push_back(seg);
    }
    GetLocalTime(&tEndLocal);
    {
        std::wstring out;
        bool repOk = WriteReport(cfg, stats, tStartLocal, tEndLocal, totalElapsed, reason, noBuf, &out);
        Dbg(cfg.stateDir, L"最终报告", repOk, repOk ? out : L"写入失败");
    }

    CloseHandle(hFile);
    Dbg(cfg.stateDir, L"关闭压力文件", true, cfg.filePath);

    if (cfg.deleteOnExit) {
        bool delOk = DeleteFileW(cfg.filePath.c_str()) == TRUE;
        Dbg(cfg.stateDir, L"删除压力文件", delOk,
            cfg.filePath + (delOk ? L"" : (L" 失败 err=" + FormatInt(GetLastError()))));
    } else {
        Dbg(cfg.stateDir, L"删除压力文件", true, L"配置为保留，跳过");
    }

    DeleteFileW((cfg.stateDir + L"\\" + PID_FILE).c_str());
    SetThreadExecutionState(ES_CONTINUOUS);
    Dbg(cfg.stateDir, L"退出", true, L"RunWorker 正常结束，返回码=0");
    return 0;
}
