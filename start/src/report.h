#pragma once
#include "shared.h"

// Fixed-bucket latency histogram: 100us per bucket, up to 10s.
struct LatencyHist {
    static const uint32_t kBucketUs = 100;
    static const uint32_t kBuckets  = 100000;

    std::vector<uint64_t> buckets;
    uint64_t total;
    uint64_t over;

    LatencyHist() : buckets(kBuckets, 0), total(0), over(0) {}
    void Add(double us) {
        if (us < 0) us = 0;
        uint64_t idx = (uint64_t)(us / (double)kBucketUs);
        if (idx >= kBuckets) { over++; }
        else { buckets[(size_t)idx]++; }
        total++;
    }
    double PercentileUs(double p) const; // p in (0,1]
};

struct Segment {
    double   startSec;
    uint64_t writes;
    uint64_t bytes;
    double   sumWriteUs;
    double   sumSyncUs;
    double   maxSyncUs;
};

struct Stats {
    uint64_t writes;
    uint64_t bytes;
    uint64_t writeErrors;
    uint64_t syncErrors;
    uint64_t allocatedBytes;   // last sampled real on-disk size
    double   sumWriteUs;
    double   sumSyncUs;
    double   minWriteUs;
    double   maxWriteUs;
    double   minSyncUs;
    double   maxSyncUs;
    LatencyHist writeHist;
    LatencyHist syncHist;
    std::vector<Segment> segments;

    Stats()
        : writes(0), bytes(0), writeErrors(0), syncErrors(0),
          allocatedBytes(0),
          sumWriteUs(0), sumSyncUs(0),
          minWriteUs(1e18), maxWriteUs(0),
          minSyncUs(1e18), maxSyncUs(0) {}

    void AddWrite(double us) {
        sumWriteUs += us;
        if (us < minWriteUs) minWriteUs = us;
        if (us > maxWriteUs) maxWriteUs = us;
        writeHist.Add(us);
    }
    void AddSync(double us) {
        sumSyncUs += us;
        if (us < minSyncUs) minSyncUs = us;
        if (us > maxSyncUs) maxSyncUs = us;
        syncHist.Add(us);
    }
};

bool WriteReport(const Config& cfg,
                 const Stats& st,
                 const SYSTEMTIME& tStart,
                 const SYSTEMTIME& tEnd,
                 double durationSec,
                 const wchar_t* reason,
                 bool   noBufferingActive,
                 std::wstring* outPath);

// Keep only the newest `keep` reports, delete the rest.
int RotateReports(const std::wstring& reportDir, int keep);
