// 核心扫描实现
#include "core_scanner.h"
#include "common.h"
#include <windows.h>

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

    // 枚举 versions 下每个子文件夹（每个子文件夹 = 一个版本）
    std::wstring pattern = versionsDir + L"\\*";
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) { cfg.SetCoreList(out); return; }

    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        std::wstring name(fd.cFileName);
        if (name == L"." || name == L"..") continue;

        std::wstring verDir = versionsDir + L"\\" + name;
        // 在该版本目录里找 >10MB 的 jar（核心文件）
        std::wstring jarPattern = verDir + L"\\*.jar";
        WIN32_FIND_DATAW jfd; HANDLE jh = FindFirstFileW(jarPattern.c_str(), &jfd);
        if (jh == INVALID_HANDLE_VALUE) continue;
        // 注意：必须在 FindClose 之前把文件名取出来 —— FindClose 后
        // jfd.cFileName 的内容不再保证有效（旧代码在关闭后才读，属未定义行为）
        std::wstring jarName;
        do {
            LARGE_INTEGER s; s.LowPart = jfd.nFileSizeLow; s.HighPart = (LONG)jfd.nFileSizeHigh;
            if (s.QuadPart > 10LL * 1024 * 1024) { // 大于 10MB 判定为核心
                jarName = jfd.cFileName;
                break;
            }
        } while (FindNextFileW(jh, &jfd));
        FindClose(jh);

        if (!jarName.empty()) {
            CoreInfo ci;
            ci.name = name;
            ci.dir = verDir;
            ci.jar = verDir + L"\\" + jarName;
            out.push_back(ci);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    cfg.SetCoreList(out);
    if (!out.empty() && cfg.selectedCore.empty())
        cfg.selectedCore = out[0].dir;
    util::Log("扫描到 " + std::to_string(out.size()) + " 个游戏核心");
}
