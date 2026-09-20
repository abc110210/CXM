#pragma once
#include "shared.h"

// Runs the 4 KiB random-write + fsync loop until one of the stop handles is signaled.
// Any handle may be NULL. Returns 0 on a clean stop, non-zero on a fatal error.
int RunWorker(const Config& cfg, HANDLE hStopA, HANDLE hStopB, HANDLE hStopC);
