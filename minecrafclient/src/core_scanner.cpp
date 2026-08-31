// 核心扫描实现
#include "core_scanner.h"
#include "common.h"
#include <windows.h>
#include <fstream>
#include <sstream>
#include "nlohmann/json.hpp"

namespace {
    bool ReadUtf8File(const std::wstring& path, std::string& out) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        std::stringstream ss; ss << f.rdbuf();
        out = ss.str();
        if (out.size() >= 3 && (unsigned char)out[0] == 0xEF &&
            (unsigned char)out[1] == 0xBB && (unsigned char)out[2] == 0xBF)
            out = out.substr(3);
        return true;
    }

    // 从字符串中提取第一个 semver：(\d+)\.(\d+)(?:\.(\d+))?
    bool ParseSemver(const std::wstring& s, int& maj, int& min, int& pat) {
        maj = min = pat = 0;
        size_t i = 0;
        while (i < s.size() && !(s[i] >= L'0' && s[i] <= L'9')) ++i;
        if (i >= s.size()) return false;
        std::wstring n;
        while (i < s.size() && s[i] >= L'0' && s[i] <= L'9') n += s[i++];
        maj = _wtoi(n.c_str());
        auto readNum = [&]() -> int {
            std::wstring x;
            while (i < s.size() && s[i] >= L'0' && s[i] <= L'9') x += s[i++];
            return _wtoi(x.c_str());
        };
        if (i < s.size() && s[i] == L'.') { ++i; min = readNum();
            if (i < s.size() && s[i] == L'.') { ++i; pat = readNum(); } }
        return maj > 0 || min > 0;
    }

    std::wstring ExtractSemver(const std::wstring& s) {
        int a, b, c;
        if (ParseSemver(s, a, b, c)) {
            std::wstring r = std::to_wstring(a) + L"." + std::to_wstring(b);
            if (c > 0) r += L"." + std::to_wstring(c);
            return r;
        }
        return {};
    }
}

std::wstring CoreScanner::ReadRealVersion(const std::wstring& verDir,
                                          const std::wstring& jarName,
                                          const std::wstring& folderName) {
    // jar 同名 json：去掉 .jar 后缀拼 .json；找不到再退回 <文件夹名>.json
    std::wstring base = jarName;
    if (base.size() > 4 && _wcsicmp(base.c_str() + base.size() - 4, L".jar") == 0)
        base = base.substr(0, base.size() - 4);
    std::wstring jsonPath = verDir + L"\\" + base + L".json";
    if (GetFileAttributesW(jsonPath.c_str()) == INVALID_FILE_ATTRIBUTES)
        jsonPath = verDir + L"\\" + folderName + L".json";

    std::string txt;
    nlohmann::json v;
    if (ReadUtf8File(jsonPath, txt)) {
        try { v = nlohmann::json::parse(txt); } catch (...) { v = nlohmann::json(); }
    }
    if (v.is_object()) {
        if (v.contains("clientVersion") && v["clientVersion"].is_string())
            return util::StringToWString(v["clientVersion"].get<std::string>());
        if (v.contains("id") && v["id"].is_string())
            return util::StringToWString(v["id"].get<std::string>());
    }
    // 回退：从文件夹名提取 semver（如 "jijizhijia-1.20.1" -> "1.20.1"）
    std::wstring fb = ExtractSemver(folderName);
    return fb;
}

int CoreScanner::RecommendJavaMajor(const std::wstring& realVersion) {
    int maj = 0, min = 0, pat = 0;
    ParseSemver(realVersion, maj, min, pat);
    if (maj <= 0) return 17;        // 解析失败：保守给 17（覆盖多数现代客户端）
    if (maj >= 2) return 21;        // 未来大版本
    // maj == 1
    if (min <= 16) return 8;        // 1.12.2 / 1.16.5 等
    if (min == 17) return 17;       // 1.17 需 Java 16/17
    if (min == 18 || min == 19 || min == 20) {
        if (min == 20 && pat >= 6) return 21;  // 1.20.6 起需 Java 21
        return 17;                            // 1.18.2 / 1.19.2 / 1.20.1/2/4
    }
    return 21;                      // 1.21.x 起
}

int CoreScanner::JavaMajorFromDisplay(const std::wstring& display) {
    int maj = 0, min = 0, pat = 0;
    ParseSemver(display, maj, min, pat);
    if (maj == 1) return min;      // "1.8.0_401" -> 8；"1.17" -> 17
    if (maj >= 2) return maj;      // "21.0.3" -> 21
    return min;                    // 兜底
}

void CoreScanner::Scan(Config& cfg) {
    std::vector<CoreInfo> out;

    // 游戏目录从 config（持久化到 ini）读取，默认与启动器同目录的 .minecraft
    std::wstring mcDir = cfg.gameDir;
    std::wstring versionsDir = mcDir + L"\\versions";
    if (GetFileAttributesW(versionsDir.c_str()) == INVALID_FILE_ATTRIBUTES) {
        util::Log("未找到 .minecraft\\versions 目录，跳过核心扫描");
        cfg.SetCoreList(out);
        return;
    }

    // 枚举 versions 下每个子文件夹（每个子文件夹 = 一个版本，可同时存在多个版本）
    std::wstring pattern = versionsDir + L"\\*";
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) { cfg.SetCoreList(out); return; }

    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        std::wstring name(fd.cFileName);
        if (name == L"." || name == L"..") continue;

        std::wstring verDir = versionsDir + L"\\" + name;
        // 在该版本目录里找核心 jar。
        // 选取优先级：① 与文件夹同名的 jar（标准布局 <文件夹名>\<文件夹名>.jar）
        //            ② 目录下最大的 jar（>1MB，过滤掉 optifine 补丁之类的小杂项）
        // 注意：这里**不能用「>10MB」硬阈值**判定核心——部分版本的核心 jar 不足 10MB
        // （例如 1.12.2 Forge+OptiFine 只有约 9.7MB），硬阈值会把这些版本整个漏掉，
        // 表现为「versions 下有版本，却报未找到核心 jar 文件」。
        std::wstring jarPattern = verDir + L"\\*.jar";
        WIN32_FIND_DATAW jfd; HANDLE jh = FindFirstFileW(jarPattern.c_str(), &jfd);
        std::wstring jarName, biggest;
        long long biggestSize = 0;
        if (jh != INVALID_HANDLE_VALUE) {
            // 注意：必须在 FindClose 之前把文件名取出来 —— FindClose 后
            // jfd.cFileName 的内容不再保证有效（旧代码在关闭后才读，属未定义行为）
            do {
                if (jfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                std::wstring fn = jfd.cFileName;
                LARGE_INTEGER s; s.LowPart = jfd.nFileSizeLow;
                s.HighPart = (LONG)jfd.nFileSizeHigh;
                long long sz = s.QuadPart;
                if (sz <= 1LL * 1024 * 1024) continue;   // 小于 1MB 的必然不是游戏核心
                std::wstring stem = fn;
                if (stem.size() > 4 && _wcsicmp(stem.c_str() + stem.size() - 4, L".jar") == 0)
                    stem = stem.substr(0, stem.size() - 4);
                if (_wcsicmp(stem.c_str(), name.c_str()) == 0) { jarName = fn; break; }
                if (sz > biggestSize) { biggestSize = sz; biggest = fn; }
            } while (FindNextFileW(jh, &jfd));
            FindClose(jh);
        }
        if (jarName.empty()) jarName = biggest;

        if (!jarName.empty()) {
            CoreInfo ci;
            ci.name = name;
            ci.dir = verDir;
            ci.jar = verDir + L"\\" + jarName;
            // 读版本 jar 同名 json 的 clientVersion，拿到真实版本（用于推荐 Java）
            ci.realVersion = ReadRealVersion(verDir, jarName, name);
            out.push_back(ci);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    cfg.SetCoreList(out);
    // 已选核心失效时自动回退：切换游戏目录（game_dir）或把启动器挪到别处后，
    // 旧的 selectedCore 路径往往已不在当前扫描结果里。此时必须自动选中第一个
    // 可用版本，否则点启动会直接报「未找到核心 jar 文件」——即使 versions 下明明有
    // 好几个版本（表现为「我明明有 4 个版本却启动不了」）。
    if (!out.empty()) {
        bool valid = false;
        for (auto& c : out) if (c.dir == cfg.selectedCore) { valid = true; break; }
        if (!valid) {
            util::Log("已选核心无效/已失效，自动回退到：" + util::WStringToString(out[0].dir));
            cfg.selectedCore = out[0].dir;
        }
    } else {
        cfg.selectedCore.clear();
    }
    util::Log("扫描到 " + std::to_string(out.size()) + " 个游戏核心");
}
