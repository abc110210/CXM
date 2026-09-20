#include "report.h"

#include <algorithm>
#include <cstdio>
#include <string>

#pragma comment(lib, "advapi32.lib")

double LatencyHist::PercentileUs(double p) const {
    if (total == 0) return 0.0;
    if (p <= 0) p = 0.0001;
    if (p > 1) p = 1.0;
    double target = p * (double)total;
    uint64_t acc = 0;
    for (size_t i = 0; i < buckets.size(); i++) {
        if (buckets[i] == 0) continue;
        if ((double)acc + (double)buckets[i] >= target) {
            double frac = (target - (double)acc) / (double)buckets[i];
            return ((double)i + frac) * (double)kBucketUs;
        }
        acc += buckets[i];
    }
    return (double)(kBuckets * kBucketUs);
}

// ---------------- system info ----------------

typedef LONG (WINAPI* RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);

static std::wstring GetOsName() {
    std::wstring out = L"Windows (unknown build)";
    HMODULE h = GetModuleHandleW(L"ntdll.dll");
    if (h) {
        RtlGetVersionPtr fn = (RtlGetVersionPtr)GetProcAddress(h, "RtlGetVersion");
        if (fn) {
            RTL_OSVERSIONINFOW vi = {0};
            vi.dwOSVersionInfoSize = sizeof(vi);
            if (fn(&vi) == 0) {
                wchar_t b[128];
                swprintf(b, 128, L"Windows NT %lu.%lu (build %lu)",
                         vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber);
                out = b;
            }
        }
    }
    return out;
}

static std::wstring GetCpuName() {
    wchar_t name[256] = {0};
    DWORD sz = sizeof(name);
    if (RegGetValueW(HKEY_LOCAL_MACHINE,
                     L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                     L"ProcessorNameString",
                     RRF_RT_REG_SZ, NULL, name, &sz) != ERROR_SUCCESS) {
        return L"unknown";
    }
    // trim trailing spaces
    std::wstring s(name);
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\0')) s.pop_back();
    return s;
}

static std::wstring GetVolumeInfo(const std::wstring& path, uint64_t* totalBytes, uint64_t* freeBytes) {
    std::wstring root;
    if (path.size() >= 2 && path[1] == L':') {
        root = path.substr(0, 2) + L"\\";
    } else {
        root = L"C:\\";
    }
    ULARGE_INTEGER t, f, totalFree;
    if (GetDiskFreeSpaceExW(root.c_str(), &f, &t, &totalFree)) {
        *totalBytes = t.QuadPart;
        *freeBytes = f.QuadPart;
    } else {
        *totalBytes = 0;
        *freeBytes = 0;
    }
    wchar_t fsName[64] = {0};
    DWORD serial = 0, maxComp = 0, flags = 0;
    std::wstring out = root;
    if (GetVolumeInformationW(root.c_str(), NULL, 0, &serial, &maxComp, &flags, fsName, 64)) {
        out += L"  [" + std::wstring(fsName) + L"]";
    }
    return out;
}

static std::wstring GetSectorSize(const std::wstring& path) {
    std::wstring root = (path.size() >= 2 && path[1] == L':') ? path.substr(0, 2) + L"\\" : L"C:\\";
    DWORD spc = 0, bps = 0, nfc = 0, tnc = 0;
    wchar_t b[64];
    if (GetDiskFreeSpaceW(root.c_str(), &spc, &bps, &nfc, &tnc)) {
        swprintf(b, 64, L"%lu bytes (cluster %lu bytes)", bps, bps * spc);
    } else {
        swprintf(b, 64, L"unknown");
    }
    return std::wstring(b);
}

// ---------------- helpers ----------------

static void KV(std::wstring& o, const wchar_t* k, const std::wstring& v) {
    o += L"  ";
    o += k;
    o += L" : ";
    o += v;
    o += L"\r\n";
}

static std::wstring UsToMs(double us) { return FormatDouble(us / 1000.0, 3); }

static std::wstring Bar(double ratio, int width) {
    int n = (int)(ratio * width + 0.5);
    if (n < 0) n = 0;
    if (n > width) n = width;
    return std::wstring((size_t)n, L'#') + std::wstring((size_t)(width - n), L'.');
}

static void AppendHistogram(std::wstring& o, const LatencyHist& h, const wchar_t* title) {
    o += L"\r\n";
    o += title;
    o += L"\r\n";
    if (h.total == 0) {
        o += L"  (no samples)\r\n";
        return;
    }
    double upperUs = h.PercentileUs(0.999) * 1.3;
    if (upperUs < 1000.0) upperUs = 1000.0;
    const int groups = 24;
    double gw = upperUs / groups;

    std::vector<uint64_t> g(groups, 0);
    for (size_t i = 0; i < h.buckets.size(); i++) {
        if (h.buckets[i] == 0) continue;
        double us = ((double)i + 0.5) * (double)LatencyHist::kBucketUs;
        size_t gi = (size_t)(us / gw);
        if (gi >= (size_t)groups) gi = groups - 1;
        g[gi] += h.buckets[i];
    }
    uint64_t gmax = 0;
    for (size_t i = 0; i < g.size(); i++) if (g[i] > gmax) gmax = g[i];

    for (int i = 0; i < groups; i++) {
        double lo = gw * i / 1000.0;
        double hi = gw * (i + 1) / 1000.0;
        wchar_t line[256];
        swprintf(line, 256, L"  %7.2f - %7.2f ms | %12s | %s\r\n",
                 lo, hi, FormatInt(g[i]).c_str(),
                 Bar(gmax ? (double)g[i] / (double)gmax : 0.0, 40).c_str());
        o += line;
    }
}

static void AppendSegments(std::wstring& o, const Stats& st) {
    o += L"\r\n";
    o += L"[6] 分段统计（每段为配置的时间窗口，默认 10 分钟）\r\n";
    o += L"--------------------------------------------------------------------------------\r\n";
    if (st.segments.empty()) {
        o += L"  (尚无完整分段)\r\n";
        return;
    }
    o += L"  #  起始(s)   写入次数      写入量     平均IOPS   平均写(ms)  平均同步(ms)  最大同步(ms)\r\n";
    for (size_t i = 0; i < st.segments.size(); i++) {
        const Segment& s = st.segments[i];
        double secs = 0;
        if (i + 1 < st.segments.size()) {
            secs = st.segments[i + 1].startSec - s.startSec;
        }
        if (secs <= 0) secs = 1.0;
        double iops = (double)s.writes / secs;
        double avgW = s.writes ? s.sumWriteUs / (double)s.writes / 1000.0 : 0.0;
        double avgS = s.writes ? s.sumSyncUs / (double)s.writes / 1000.0 : 0.0;
        wchar_t line[320];
        swprintf(line, 320,
                 L"  %-3zu %9.0f  %11s  %10s  %9.2f  %11.3f  %12.3f  %12.3f\r\n",
                 i + 1, s.startSec,
                 FormatInt(s.writes).c_str(), FormatBytes(s.bytes).c_str(),
                 iops, avgW, avgS, s.maxSyncUs / 1000.0);
        o += line;
    }
}

bool WriteReport(const Config& cfg,
                 const Stats& st,
                 const SYSTEMTIME& tStart,
                 const SYSTEMTIME& tEnd,
                 double durationSec,
                 const wchar_t* reason,
                 bool noBufferingActive,
                 std::wstring* outPath) {
    if (!EnsureDir(cfg.reportDir)) return false;

    std::wstring name = REPORT_PREFIX + FormatStamp(tEnd) + REPORT_EXT;
    std::wstring path = cfg.reportDir + L"\\" + name;

    std::wstring o;
    o += L"================================================================================\r\n";
    o += L" DiskStress - 4KiB 随机写入 + 每次 fsync 压力测试报告\r\n";
    o += L"================================================================================\r\n";

    o += L"\r\n[1] 基本信息\r\n--------------------------------------------------------------------------------\r\n";
    KV(o, L"报告生成时间", FormatTime(tEnd));
    KV(o, L"进程启动时间", FormatTime(tStart));
    KV(o, L"本次运行时长", FormatDuration(durationSec) + L"  (" + FormatDouble(durationSec, 1) + L" s)");
    KV(o, L"报告产生原因", reason);
    KV(o, L"进程 PID   ", FormatInt(GetCurrentProcessId()));
    KV(o, L"程序版本   ", APP_VER + std::wstring(L" (build " ) + FormatTime(tEnd) + L")");
    KV(o, L"报告文件   ", path);

    o += L"\r\n[2] 测试配置\r\n--------------------------------------------------------------------------------\r\n";
    KV(o, L"压力文件路径", cfg.filePath);
    KV(o, L"固定覆盖区  ", FormatBytes(cfg.workingSetBytes) + L"  (" + FormatInt(cfg.workingSetBytes) +
                        L" bytes) 随机偏移范围 = 占用上限");
    KV(o, L"占用控制    ", std::wstring(L"覆盖写入：文件长度固定，只在已有区间内反复覆盖，不会增长"));
    KV(o, L"退出时删除  ", cfg.deleteOnExit ? L"是（停止后压力文件被删除，占用归零）" : L"否");
    KV(o, L"单次写入块  ", FormatBytes(cfg.blockBytes) + L"  (" + FormatInt(cfg.blockBytes) + L" bytes)");
    KV(o, L"写入方式    ", std::wstring(L"随机偏移，每块写满后调用 FlushFileBuffers (fsync)"));
    KV(o, L"缓存策略    ", noBufferingActive ? L"FILE_FLAG_NO_BUFFERING（绕过系统缓存，直接落盘）"
                                             : L"系统缓存模式（NO_BUFFERING 不可用，已回退）");
    KV(o, L"报告周期    ", FormatInt(cfg.reportIntervalSec / 60) + L" 分钟 (" + FormatInt(cfg.reportIntervalSec) + L" s)");
    KV(o, L"分段窗口    ", FormatInt(cfg.segmentSec / 60) + L" 分钟");
    KV(o, L"报告目录    ", cfg.reportDir + L"  (最多保留 3 份)");

    o += L"\r\n[3] 系统与环境信息\r\n--------------------------------------------------------------------------------\r\n";
    KV(o, L"操作系统    ", GetOsName());
    KV(o, L"CPU         ", GetCpuName());
    {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        KV(o, L"CPU 线程数  ", FormatInt(si.dwNumberOfProcessors));
    }
    {
        MEMORYSTATUSEX ms;
        ms.dwLength = sizeof(ms);
        if (GlobalMemoryStatusEx(&ms)) {
            KV(o, L"物理内存    ", FormatBytes(ms.ullTotalPhys) + L"  可用 " + FormatBytes(ms.ullAvailPhys) +
                                L"  使用率 " + FormatDouble((double)ms.dwMemoryLoad, 1) + L"%");
        }
    }
    {
        uint64_t total = 0, free = 0;
        std::wstring vol = GetVolumeInfo(cfg.filePath, &total, &free);
        KV(o, L"目标卷      ", vol);
        KV(o, L"卷容量/可用 ", FormatBytes(total) + L" / " + FormatBytes(free));
        KV(o, L"扇区大小    ", GetSectorSize(cfg.filePath));
    }
    {
        wchar_t user[256] = {0};
        DWORD n = 256;
        if (GetUserNameW(user, &n)) KV(o, L"运行账户    ", std::wstring(user));
        KV(o, L"管理员权限  ", L"(按实际运行方式而定)");
    }

    o += L"\r\n[4] 写入统计总览\r\n--------------------------------------------------------------------------------\r\n";
    double secs = durationSec > 0 ? durationSec : 1.0;
    KV(o, L"总写入次数  ", FormatInt(st.writes));
    KV(o, L"总写入字节  ", FormatBytes(st.bytes) + L"  (" + FormatInt(st.bytes) + L" bytes)");
    KV(o, L"实际占用(采样)", FormatBytes(st.allocatedBytes) + L"  (上限 " +
                          FormatBytes(cfg.workingSetBytes) + L"，覆盖写不会超过)");
    KV(o, L"平均 IOPS   ", FormatDouble((double)st.writes / secs, 3) + L" ops/s");
    KV(o, L"平均吞吐    ", FormatDouble((double)st.bytes / secs / (1024.0 * 1024.0), 3) + L" MiB/s (含 fsync 等待)");
    KV(o, L"写入延迟    ", L"平均 " + UsToMs(st.writes ? st.sumWriteUs / st.writes : 0) +
                        L" ms | 最小 " + UsToMs(st.writes ? st.minWriteUs : 0) +
                        L" ms | 最大 " + UsToMs(st.maxWriteUs) + L" ms");
    KV(o, L"fsync 延迟  ", L"平均 " + UsToMs(st.writes ? st.sumSyncUs / st.writes : 0) +
                        L" ms | 最小 " + UsToMs(st.writes ? st.minSyncUs : 0) +
                        L" ms | 最大 " + UsToMs(st.maxSyncUs) + L" ms");

    o += L"\r\n[5] fsync 延迟分位数 (ms)\r\n--------------------------------------------------------------------------------\r\n";
    {
        wchar_t line[320];
        swprintf(line, 320,
                 L"  P50 %8s | P90 %8s | P95 %8s | P99 %8s | P99.9 %8s | MAX %8s\r\n",
                 UsToMs(st.syncHist.PercentileUs(0.50)).c_str(),
                 UsToMs(st.syncHist.PercentileUs(0.90)).c_str(),
                 UsToMs(st.syncHist.PercentileUs(0.95)).c_str(),
                 UsToMs(st.syncHist.PercentileUs(0.99)).c_str(),
                 UsToMs(st.syncHist.PercentileUs(0.999)).c_str(),
                 UsToMs(st.maxSyncUs).c_str());
        o += line;
    }
    AppendHistogram(o, st.syncHist, L"[5.1] fsync 延迟分布（横轴 ms，纵轴样本数）");
    o += L"\r\n[5.2] WriteFile 延迟分位数 (ms)  P50 " +
         UsToMs(st.writeHist.PercentileUs(0.50)) + L" | P95 " +
         UsToMs(st.writeHist.PercentileUs(0.95)) + L" | P99 " +
         UsToMs(st.writeHist.PercentileUs(0.99)) + L"\r\n";

    AppendSegments(o, st);

    o += L"\r\n[7] 错误与异常\r\n--------------------------------------------------------------------------------\r\n";
    KV(o, L"WriteFile 失败", FormatInt(st.writeErrors));
    KV(o, L"Flush 失败    ", FormatInt(st.syncErrors));
    KV(o, L"磁盘空间告警  ", (st.writeErrors || st.syncErrors)
                            ? L"存在 I/O 错误，请结合系统事件日志排查"
                            : L"无");

    o += L"\r\n[8] 说明\r\n--------------------------------------------------------------------------------\r\n";
    o += L"  * 每次 4KiB 写入后立即 fsync，吞吐受限于单次落盘延迟，数值偏低属正常现象。\r\n";
    o += L"  * 工作集大于 SSD 缓存时，IOPS 曲线跌阶可反映 SLC 缓存耗尽后的真实颗粒写入性能。\r\n";
    o += L"  * 分段统计表可用于观察缓内 / 缓外两个阶段的性能变化。\r\n";
    o += L"  * 磁盘占用采用「固定覆盖区」：压力文件放在系统 TMP 目录，长度固定，随机偏移只在\r\n";
    o += L"    该区间内反复覆盖，文件不会增长，真实占用最多等于覆盖区大小。\r\n";
    o += L"  * 压力文件为专用测试文件，请勿指向任何有用数据文件，程序会按工作集大小重置其长度。\r\n";
    o += L"\r\n================================ 报告结束 ========================================\r\n";

    bool ok = WriteTextFileUtf8(path, o);
    if (ok) {
        RotateReports(cfg.reportDir, (int)kMaxReports);
        if (outPath) *outPath = path;
    }
    return ok;
}

int RotateReports(const std::wstring& reportDir, int keep) {
    std::wstring pattern = reportDir + L"\\" + REPORT_PREFIX + L"*" + REPORT_EXT;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;

    struct Item {
        std::wstring name;
        FILETIME ft;
    };
    std::vector<Item> items;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        items.push_back({std::wstring(fd.cFileName), fd.ftCreationTime});
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    if ((int)items.size() <= keep) return 0;

    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        if (a.name.size() == b.name.size()) return a.name > b.name; // newer stamp first
        return a.name > b.name;
    });

    int removed = 0;
    for (size_t i = (size_t)keep; i < items.size(); i++) {
        std::wstring p = reportDir + L"\\" + items[i].name;
        if (DeleteFileW(p.c_str())) removed++;
    }
    return removed;
}
