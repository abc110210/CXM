#pragma once
// 配置文件读写（简单 INI 实现，不依赖第三方库）
// 配置文件 config.ini 与 exe 同目录，避免每次启动都扫硬盘
#include <string>
#include <map>
#include <vector>

struct JavaInfo {
    std::wstring display;   // 显示名，如 "Java 21.0.3 (Adoptium)"
    std::wstring javaExe;   // java.exe 完整路径
};

struct CoreInfo {
    std::wstring name;      // 版本名（文件夹名）
    std::wstring dir;       // 版本目录
    std::wstring jar;       // 核心 jar 完整路径
};

class Config {
public:
    Config();

    // 从 ini 读取所有缓存
    void Load();
    // 写回 ini
    void Save();

    // ---- 基础设置 ----
    std::wstring clientId;
    std::wstring clientSecret;   // 加密存储，这里先用明文占位（生产建议 DPAPI）
    std::wstring redirectUri;

    // ---- 扫描结果缓存 ----
    std::vector<JavaInfo> javaList;
    std::vector<CoreInfo> coreList;
    std::wstring selectedJava;
    std::wstring selectedCore;
    int memoryMb = 8192;

    // ---- 游戏目录（.minecraft 所在）----
    // 持久化到 ini，避免每次启动都去猜/扫盘；默认与启动器同目录
    std::wstring gameDir;

    // ---- 登录缓存 ----
    std::wstring accessToken;     // LittleSkin 返回的 access token
    std::wstring refreshToken;
    std::wstring currentRole;

    // ---- 云端同步 ----
    std::wstring serverUrl;       // 你的时长同步服务端地址
    std::wstring serverSecret;    // 与服务端一致的密钥

    // ---- 工具 ----
    std::wstring GetConfigPath() const;
    void SetJavaList(const std::vector<JavaInfo>& list);
    void SetCoreList(const std::vector<CoreInfo>& list);

private:
    std::wstring path_;
    void WriteSection(const std::wstring& file, const std::wstring& section,
                      const std::vector<std::wstring>& lines);
};
