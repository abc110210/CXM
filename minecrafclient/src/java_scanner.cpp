// Java 扫描实现
#include "java_scanner.h"
#include "common.h"
#include <windows.h>
#include <cstdio>
#include <algorithm>

void JavaScanner::AddIfValid(std::vector<JavaInfo>& out, const std::wstring& javaExe) {
    if (javaExe.empty()) return;
    // 必须是 java.exe 且文件存在
    if (javaExe.rfind(L"java.exe") == std::wstring::npos) return;
    if (GetFileAttributesW(javaExe.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    JavaInfo info;
    info.javaExe = javaExe;
    info.display = QueryVersion(javaExe);
    // 去重
    for (auto& j : out) if (_wcsicmp(j.javaExe.c_str(), javaExe.c_str()) == 0) return;
    out.push_back(info);
}

std::wstring JavaScanner::QueryVersion(const std::wstring& javaExe) {
    // 执行 "java -version" 并把 stderr 合并到 stdout 读取
    std::wstring cmd = L"\"" + javaExe + L"\" -version 2>&1";
    FILE* pipe = _wpopen(cmd.c_str(), L"r");
    std::wstring line;
    if (pipe) {
        wchar_t buf[512];
        while (fgetws(buf, 512, pipe)) line += buf;
        _pclose(pipe);
    }
    if (line.empty()) return javaExe.substr(javaExe.rfind(L"\\") + 1);
    // 取第一行，形如 "java version "1.8.0_401"" 或 "openjdk version "21.0.3""
    size_t nl = line.find(L'\n');
    std::wstring first = (nl == std::wstring::npos) ? line : line.substr(0, nl);
    // 去掉首尾空白
    size_t s = first.find_first_not_of(L" \t\r\n");
    size_t e = first.find_last_not_of(L" \t\r\n");
    if (s != std::wstring::npos) first = first.substr(s, e - s + 1);
    return first;
}

void JavaScanner::ScanEnvJava(std::vector<JavaInfo>& out) {
    wchar_t buf[32768] = {0};
    DWORD n = GetEnvironmentVariableW(L"JAVA_HOME", buf, 32768);
    if (n > 0 && n < 32768) {
        std::wstring home(buf);
        if (home.back() == L'\\') home.pop_back();
        AddIfValid(out, home + L"\\bin\\java.exe");
    }
}

void JavaScanner::ScanRegistry(std::vector<JavaInfo>& out) {
    // 常见发行版注册表根键
    const wchar_t* roots[] = {
        L"SOFTWARE\\JavaSoft\\Java Runtime Environment",
        L"SOFTWARE\\JavaSoft\\JDK",
        L"SOFTWARE\\Eclipse Adoptium\\JDK",
        L"SOFTWARE\\AdoptOpenJDK",
        L"SOFTWARE\\Azul Systems\\Zulu",
        L"SOFTWARE\\Amazon Corretto"
    };
    for (const wchar_t* root : roots) {
        HKEY hKey = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, root, 0, KEY_READ | KEY_WOW64_64KEY, &hKey) != ERROR_SUCCESS)
            continue;
        // 尝试读 JavaHome（有些直接挂在根，有些挂在版本子键）
        wchar_t home[MAX_PATH] = {0};
        DWORD sz = MAX_PATH;
        if (RegQueryValueExW(hKey, L"JavaHome", nullptr, nullptr, (LPBYTE)home, &sz) == ERROR_SUCCESS) {
            std::wstring h(home);
            if (h.back() == L'\\') h.pop_back();
            AddIfValid(out, h + L"\\bin\\java.exe");
        }
        // 枚举子键（版本）
        wchar_t sub[256]; DWORD subSz;
        for (DWORD i = 0; ; ++i) {
            subSz = 256;
            if (RegEnumKeyExW(hKey, i, sub, &subSz, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            HKEY hSub = nullptr;
            if (RegOpenKeyExW(hKey, sub, 0, KEY_READ | KEY_WOW64_64KEY, &hSub) == ERROR_SUCCESS) {
                wchar_t jh[MAX_PATH] = {0}; DWORD jsz = MAX_PATH;
                if (RegQueryValueExW(hSub, L"JavaHome", nullptr, nullptr, (LPBYTE)jh, &jsz) == ERROR_SUCCESS) {
                    std::wstring h(jh);
                    if (h.back() == L'\\') h.pop_back();
                    AddIfValid(out, h + L"\\bin\\java.exe");
                }
                RegCloseKey(hSub);
            }
        }
        RegCloseKey(hKey);
    }
}

void JavaScanner::ScanCommonDirs(std::vector<JavaInfo>& out) {
    const wchar_t* dirs[] = {
        L"C:\\Program Files\\Java\\*",
        L"C:\\Program Files (x86)\\Java\\*",
        L"C:\\Program Files\\Eclipse Adoptium\\*",
        L"C:\\Program Files\\Microsoft\\*",
        L"C:\\Program Files\\Amazon Corretto\\*",
        L"%LOCALAPPDATA%\\Programs\\Eclipse Adoptium\\*"
    };
    for (const wchar_t* pat : dirs) {
        std::wstring expanded = pat;
        // 展开 %LOCALAPPDATA%
        if (expanded.find(L"%LOCALAPPDATA%") != std::wstring::npos) {
            wchar_t la[MAX_PATH] = {0};
            if (GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH))
                expanded.replace(expanded.find(L"%LOCALAPPDATA%"), 14, la);
        }
        WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(expanded.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        std::wstring base = expanded.substr(0, expanded.find_last_of(L"\\") + 1);
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                std::wstring name(fd.cFileName);
                if (name == L"." || name == L"..") continue;
                AddIfValid(out, base + name + L"\\bin\\java.exe");
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

void JavaScanner::ScanPath(std::vector<JavaInfo>& out) {
    wchar_t pathBuf[32768] = {0};
    DWORD n = GetEnvironmentVariableW(L"PATH", pathBuf, 32768);
    if (n == 0) return;
    std::wstring path(pathBuf);
    size_t start = 0;
    while (true) {
        size_t end = path.find(L";", start);
        std::wstring seg = (end == std::wstring::npos) ? path.substr(start) : path.substr(start, end - start);
        if (!seg.empty() && seg.back() != L'\\') seg += L"\\";
        AddIfValid(out, seg + L"java.exe");
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
}

void JavaScanner::Scan(Config& cfg) {
    std::vector<JavaInfo> out;
    ScanEnvJava(out);
    ScanRegistry(out);
    ScanCommonDirs(out);
    ScanPath(out);
    cfg.SetJavaList(out);
    if (!out.empty() && cfg.selectedJava.empty())
        cfg.selectedJava = out[0].javaExe;
    util::Log("扫描到 " + std::to_string(out.size()) + " 个 Java");
}

std::wstring JavaScanner::PickManual(const std::wstring& binDir, Config& cfg) {
    std::wstring exe = binDir;
    if (exe.back() == L'\\') exe.pop_back();
    exe += L"\\java.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES)
        return L"";  // 该目录没有 java.exe
    // 加入列表（去重在 AddIfValid 内做）
    std::vector<JavaInfo> tmp = cfg.javaList;
    AddIfValid(tmp, exe);
    cfg.SetJavaList(tmp);
    cfg.selectedJava = exe;
    return exe;
}
