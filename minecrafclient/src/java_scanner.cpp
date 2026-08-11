// Java 扫描实现
#include "java_scanner.h"
#include "common.h"
#include <windows.h>
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

// 取 java.exe 所在目录名作为显示名兜底（如 zulu21.48.17-ca-jdk21.0.10-win_x64），
// 比裸的 "java.exe" 有用得多
static std::wstring FallbackName(const std::wstring& javaExe) {
    std::wstring dir = javaExe.substr(0, javaExe.rfind(L'\\'));
    size_t p = dir.find_last_of(L'\\');
    std::wstring name = (p == std::wstring::npos) ? dir : dir.substr(p + 1);
    return name.empty() ? L"java.exe" : (L"Java (" + name + L")");
}

std::wstring JavaScanner::QueryVersion(const std::wstring& javaExe) {
    // 后台运行 "java -version" 并读取输出（stderr 并入）。
    // 注意：不能用 _wpopen/system —— 那会为子进程新建一个控制台窗口，
    // 首次扫描有几个 Java 就闪几个黑窗。改用 CreateProcess + CREATE_NO_WINDOW
    // 隐藏运行，结果通过管道读回（与 game_launcher 启动游戏同款方式）。
    HANDLE hRead = nullptr, hWrite = nullptr;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    if (!CreatePipe(&hRead, &hWrite, &sa, 0))
        return FallbackName(javaExe);
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);  // 读端不继承

    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hWrite;
    si.hStdError  = hWrite;   // java -version 输出到 stderr，合并到同一管道
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    si.wShowWindow = SW_HIDE;

    // 命令行：路径用一对引号包裹即可（CreateProcess 解析规则：首 token 引号内为应用名，
    // 其余为参数）。旧写法在整条命令外加一层引号，Windows 命令行解析器对
    // ""C:\path\java.exe" -version" 这种双重引号兼容性差，会导致 CreateProcess 失败
    // 或参数解析错误，最终显示名退化成 "java.exe"。
    std::wstring cmdLine = L"\"" + javaExe + L"\" -version";
    PROCESS_INFORMATION pi = { 0 };
    if (!CreateProcessW(nullptr, &cmdLine[0], nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(hRead);
        CloseHandle(hWrite);
        return FallbackName(javaExe);
    }
    CloseHandle(hWrite);  // 父进程关闭写端，否则 ReadFile 读不到 EOF 会一直阻塞

    std::wstring line;
    char buf[512];
    DWORD got = 0;
    while (ReadFile(hRead, buf, sizeof(buf) - 1, &got, nullptr) && got > 0) {
        buf[got] = '\0';
        // 优先按 UTF-8 解码，失败再按系统 ANSI 代码页（旧版 Java 输出可能是 GBK）
        int need = MultiByteToWideChar(CP_UTF8, 0, buf, got, nullptr, 0);
        if (need > 0) {
            std::wstring w(need, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, buf, got, &w[0], need);
            line += w;
        } else {
            int n2 = MultiByteToWideChar(CP_ACP, 0, buf, got, nullptr, 0);
            if (n2 > 0) {
                std::wstring w(n2, L'\0');
                MultiByteToWideChar(CP_ACP, 0, buf, got, &w[0], n2);
                line += w;
            }
        }
    }
    CloseHandle(hRead);
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (line.empty()) return FallbackName(javaExe);
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
