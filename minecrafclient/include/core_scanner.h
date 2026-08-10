#pragma once
// 核心扫描：启动器与 .minecraft 同目录，扫描 versions 下 >10MB 的 jar 作为核心
#include <string>
#include <vector>
#include "config.h"

class CoreScanner {
public:
    // 扫描 .minecraft/versions 下的核心并填充 Config
    static void Scan(Config& cfg);
};
