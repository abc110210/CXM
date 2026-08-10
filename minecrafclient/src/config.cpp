// 配置文件读写实现，使用 Windows 自带 INI API（最简单可靠）
#include "config.h"
#include "common.h"
#include <windows.h>
#include <shlwapi.h>

Config::Config() {
    path_ = util::GetExeDir() + L"\\config.ini";
    // 默认值
    clientId = L"1493";
    redirectUri = L"stardust://oauth/callback";
    serverUrl = L"https://你的域名.com";   // 改成你的备案域名
    serverSecret = L"请改成服务端 config.json 里的 secret_key";
}

std::wstring Config::GetConfigPath() const { return path_; }

void Config::Load() {
    auto getStr = [&](const wchar_t* sec, const wchar_t* key, const std::wstring& def) -> std::wstring {
        wchar_t buf[2048] = {0};
        DWORD n = GetPrivateProfileStringW(sec, key, def.c_str(), buf, 2048, path_.c_str());
        return std::wstring(buf, n);
    };
    auto getInt = [&](const wchar_t* sec, const wchar_t* key, int def) {
        return GetPrivateProfileIntW(sec, key, def, path_.c_str());
    };

    clientId     = getStr(L"launcher", L"client_id", clientId);
    clientSecret = getStr(L"launcher", L"client_secret", clientSecret);
    redirectUri  = getStr(L"launcher", L"redirect_uri", redirectUri);
    selectedJava = getStr(L"launcher", L"selected_java", selectedJava);
    selectedCore = getStr(L"launcher", L"selected_core", selectedCore);
    memoryMb     = getInt(L"launcher", L"memory_mb", memoryMb);
    currentRole  = getStr(L"launcher", L"selected_role", currentRole);

    accessToken  = getStr(L"auth", L"access_token", accessToken);
    refreshToken = getStr(L"auth", L"refresh_token", refreshToken);

    serverUrl    = getStr(L"cloud", L"server_url", serverUrl);
    serverSecret = getStr(L"cloud", L"server_secret", serverSecret);

    // 读 Java 列表
    int javaCount = getInt(L"java", L"count", 0);
    javaList.clear();
    for (int i = 0; i < javaCount; ++i) {
        std::wstring idx = std::to_wstring(i);
        JavaInfo j;
        j.display = getStr(L"java", (L"java" + idx + L"_name").c_str(), L"");
        j.javaExe = getStr(L"java", (L"java" + idx + L"_exe").c_str(), L"");
        if (!j.javaExe.empty()) javaList.push_back(j);
    }
    // 读 Core 列表
    int coreCount = getInt(L"cores", L"count", 0);
    coreList.clear();
    for (int i = 0; i < coreCount; ++i) {
        std::wstring idx = std::to_wstring(i);
        CoreInfo c;
        c.name = getStr(L"cores", (L"core" + idx + L"_name").c_str(), L"");
        c.dir  = getStr(L"cores", (L"core" + idx + L"_dir").c_str(), L"");
        c.jar  = getStr(L"cores", (L"core" + idx + L"_jar").c_str(), L"");
        if (!c.jar.empty()) coreList.push_back(c);
    }
}

void Config::Save() {
    auto setStr = [&](const wchar_t* sec, const wchar_t* key, const std::wstring& v) {
        WritePrivateProfileStringW(sec, key, v.c_str(), path_.c_str());
    };
    auto setInt = [&](const wchar_t* sec, const wchar_t* key, int v) {
        WritePrivateProfileStringW(sec, key, std::to_wstring(v).c_str(), path_.c_str());
    };

    setStr(L"launcher", L"client_id", clientId);
    setStr(L"launcher", L"client_secret", clientSecret);
    setStr(L"launcher", L"redirect_uri", redirectUri);
    setStr(L"launcher", L"selected_java", selectedJava);
    setStr(L"launcher", L"selected_core", selectedCore);
    setInt(L"launcher", L"memory_mb", memoryMb);
    setStr(L"launcher", L"selected_role", currentRole);

    setStr(L"auth", L"access_token", accessToken);
    setStr(L"auth", L"refresh_token", refreshToken);

    setStr(L"cloud", L"server_url", serverUrl);
    setStr(L"cloud", L"server_secret", serverSecret);

    setInt(L"java", L"count", (int)javaList.size());
    for (size_t i = 0; i < javaList.size(); ++i) {
        std::wstring idx = std::to_wstring(i);
        setStr(L"java", (L"java" + idx + L"_name").c_str(), javaList[i].display);
        setStr(L"java", (L"java" + idx + L"_exe").c_str(), javaList[i].javaExe);
    }
    setInt(L"cores", L"count", (int)coreList.size());
    for (size_t i = 0; i < coreList.size(); ++i) {
        std::wstring idx = std::to_wstring(i);
        setStr(L"cores", (L"core" + idx + L"_name").c_str(), coreList[i].name);
        setStr(L"cores", (L"core" + idx + L"_dir").c_str(), coreList[i].dir);
        setStr(L"cores", (L"core" + idx + L"_jar").c_str(), coreList[i].jar);
    }
}

void Config::SetJavaList(const std::vector<JavaInfo>& list) { javaList = list; }
void Config::SetCoreList(const std::vector<CoreInfo>& list) { coreList = list; }
