// 主应用实现：窗口、消息分发、模块协调
#include "app.h"
#include "resource.h"   // IDI_APP_ICON 等内嵌资源 ID
#include "common.h"
#include "oauth.h"
#include "java_scanner.h"
#include "core_scanner.h"
#include <windows.h>
#include <shlobj.h>      // IFileDialog
#include <dwmapi.h>      // DwmSetWindowAttribute（Windows 11 圆角）
#include <string>
#include <chrono>
#include <sstream>

// 用户要求：云端同步暂不启用，仅本地记录游玩时长
constexpr bool CLOUD_SYNC_ENABLED = false;

// 兼容旧版 Windows SDK：DWM 圆角属性
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif

// 从 JSON 字符串里取某个字符串字段的值（简单解析，避免引入 JSON 库）
static std::string ExtractString(const std::string& json, const std::string& key) {
    std::string target = "\"" + key + "\"";
    size_t pos = json.find(target);
    if (pos == std::string::npos) return {};
    size_t colon = json.find(':', pos);
    if (colon == std::string::npos) return {};
    size_t q1 = json.find('"', colon + 1);
    if (q1 == std::string::npos) return {};
    size_t q2 = json.find('"', q1 + 1);
    if (q2 == std::string::npos) return {};
    return json.substr(q1 + 1, q2 - q1 - 1);
}

static long long ExtractInt(const std::string& json, const std::string& key) {
    std::string target = "\"" + key + "\"";
    size_t pos = json.find(target);
    if (pos == std::string::npos) return 0;
    size_t colon = json.find(':', pos);
    if (colon == std::string::npos) return 0;
    size_t start = colon + 1;
    size_t end = json.find_first_of(",}", start);
    try { return std::stoll(json.substr(start, end - start)); }
    catch (...) { return 0; }
}

static std::wstring JsonEscape(const std::wstring& s) {
    std::wstring out;
    for (wchar_t c : s) {
        if (c == L'"' || c == L'\\') out += L'\\';
        out += c;
    }
    return out;
}

// 从 URI 的 Query String 取某个参数（不做完整 URL decode，仅用于错误展示）
static std::wstring GetQueryParam(const std::wstring& uri, const std::wstring& key) {
    std::wstring prefix1 = L"?" + key + L"=";
    std::wstring prefix2 = L"&" + key + L"=";
    size_t pos = uri.find(prefix1);
    if (pos == std::wstring::npos) pos = uri.find(prefix2);
    if (pos == std::wstring::npos) return {};
    pos += (uri[pos] == L'?' ? prefix1.size() : prefix2.size());
    size_t end = uri.find(L'&', pos);
    return (end == std::wstring::npos) ? uri.substr(pos) : uri.substr(pos, end - pos);
}

// ---------------- 初始化 ----------------
bool App::Init(HINSTANCE hInstance, int nCmdShow) {
    hinst_ = hInstance;
    nCmdShow_ = nCmdShow;

    // 读取配置（含上次扫描缓存）
    cfg_.Load();

    // 首次或缓存为空时扫描 Java / 核心
    if (cfg_.javaList.empty()) JavaScanner::Scan(cfg_);
    if (cfg_.coreList.empty()) CoreScanner::Scan(cfg_);
    cfg_.Save();

    cloud_ = new CloudSync(cfg_.serverUrl, cfg_.serverSecret);

    // 注册窗口类（WNDCLASSEXW 才有 hIconSm 成员）
    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"StardustLauncher";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    // 窗口图标 + 任务栏图标（从 exe 资源 IDI_APP_ICON 取，自动选合适尺寸；
    // ico 最小 32x32，Windows 需要更小时自动缩放）
    wc.hIcon   = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON));
    wc.hIconSm = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 32, 32, 0);
    RegisterClassExW(&wc);

    // 无边框、无系统标题栏的原生应用窗口：
    //   - WS_POPUP：去掉 WS_THICKFRAME 后窗口不可手动拉伸（用户要求）；
    //     顶部不再有 DWM 的"假标题栏"白条，WebView2 铺满整个客户区。
    //   - WS_EX_APPWINDOW：让 WS_POPUP 窗口也出现在任务栏和 Alt-Tab 列表，
    //     否则最小化后无法从任务栏还原。
    // 拖动由 HTML 头部 .titlebar 的 -webkit-app-region:drag 实现（点 winbtn 因
    // app-region:no-drag 仍可点击）。
    hwnd_ = CreateWindowExW(WS_EX_APPWINDOW, L"StardustLauncher", L"寄寄之家启动器",
                            WS_POPUP,
                            0, 0, 1280, 800,
                            nullptr, nullptr, hInstance, this);
    if (!hwnd_) return false;

    // 固定 1280×800，在主屏工作区居中显示（去掉任务栏区域），原生应用基本款
    RECT wa = {0};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int sw = wa.right - wa.left, sh = wa.bottom - wa.top;
    int x = wa.left + (sw - 1280) / 2;
    int y = wa.top  + (sh - 800)  / 2;
    SetWindowPos(hwnd_, nullptr, x, y, 1280, 800, SWP_NOZORDER | SWP_NOACTIVATE);

    ShowWindow(hwnd_, nCmdShow);
    UpdateWindow(hwnd_);

    // Windows 11 启用系统圆角，强化原生感
    DWORD corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));

    // 初始化 WebView2（网页已内嵌进 exe 资源，由 host 自动加载渲染）
    HRESULT hrInit = webview_.Init(hwnd_,
        [this](const std::wstring& json) { OnWebMessage(json); },
        [this](const std::wstring& uri)  { OnNavigate(uri); return true; },
        [this]() { // onReady
            // 把系统信息推给前端
            SendJavaList();
            SendCoreList();

            MEMORYSTATUSEX mem = { sizeof(mem) };
            GlobalMemoryStatusEx(&mem);
            double totalGB = (double)mem.ullTotalPhys / (1024.0 * 1024 * 1024);
            int total = (int)(totalGB + 0.5);                 // 四舍五入取整
            int recommended = total / 2;                      // 推荐分配约一半物理内存
            if (recommended < 2) recommended = 2;             // 至少 2G
            if (recommended > 16) recommended = 16;           // 封顶 16G
            std::wstring memJson = L"{\"type\":\"sys:memory\",\"totalGB\":" +
                std::to_wstring(total) + L",\"recommendedG\":" +
                std::to_wstring(recommended) + L"}";
            SendToJs(memJson);
        },
        [this]() { // onNavigated
            if (pendingLoginSuccess_) {
                pendingLoginSuccess_ = false;
                webview_.ExecuteScript(L"loginSuccess('" + pendingUsername_ + L"')");
            }
        });
    if (FAILED(hrInit)) {
        // 给用户一个明确提示，避免再次出现"白屏但不知为何"
        MessageBoxW(hwnd_,
            L"启动器界面初始化失败。\n可能原因：内嵌 HTML 资源缺失或 WebView2 运行库加载失败。\n请查看控制台/日志获取详细错误。",
            L"寄寄之家启动器", MB_OK | MB_ICONERROR);
        return false;
    }

    return true;
}

// ---------------- 窗口过程 ----------------
LRESULT CALLBACK App::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    App* app = nullptr;
    if (m == WM_NCCREATE) {
        CREATESTRUCT* cs = (CREATESTRUCT*)l;
        app = (App*)cs->lpCreateParams;
        SetWindowLongPtr(h, GWLP_USERDATA, (LONG_PTR)app);
    } else {
        app = (App*)GetWindowLongPtr(h, GWLP_USERDATA);
    }
    if (!app) return DefWindowProc(h, m, w, l);

    switch (m) {
        case WM_SIZE:
            app->webview_.Resize();
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_NCHITTEST: {
            // 顶部标题栏区域可拖动（留出右上角按钮区给 WebView 处理点击）
            POINT pt; GetCursorPos(&pt); ScreenToClient(h, &pt);
            RECT r; GetClientRect(h, &r);
            if (pt.y < 44 && pt.x < r.right - 90) return HTCAPTION;
            return DefWindowProc(h, m, w, l);
        }
        case WM_NCDESTROY:
            SetWindowLongPtr(h, GWLP_USERDATA, 0);
            return DefWindowProc(h, m, w, l);
    }
    return DefWindowProc(h, m, w, l);
}

// ---------------- 处理 JS 消息 ----------------
void App::SendToJs(const std::wstring& json) {
    webview_.PostMessage(json);
}

void App::OnWebMessage(const std::wstring& jsonW) {
    std::string json = util::WStringToString(jsonW);
    std::string type = ExtractString(json, "type");

    if (type == "oauth:login") {
        DoOAuthLogin();
    }
    else if (type == "role:change") {
        cfg_.currentRole = util::StringToWString(ExtractString(json, "name"));
        cfg_.Save();
    }
    else if (type == "config:set") {
        std::string key = ExtractString(json, "key");
        if (key == "memory_mb") { cfg_.memoryMb = (int)ExtractInt(json, "value"); cfg_.Save(); }
    }
    else if (type == "java:browse") {
        SelectJavaFolder();
    }
    else if (type == "game:launch") {
        OnJsLaunchGame();
    }
    else if (type == "window") {
        std::string action = ExtractString(json, "action");
        if (action == "min") ShowWindow(hwnd_, SW_MINIMIZE);
        else if (action == "close") DestroyWindow(hwnd_);
    }
    else if (type == "browser:open") {
        std::wstring url = util::StringToWString(ExtractString(json, "url"));
        ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    else if (type == "cloud:playtime:get") {
        std::wstring pid = util::StringToWString(ExtractString(json, "player_id"));
        long long total = cloud_->GetPlaytime(pid);
        if (total >= 0) {
            SendToJs(L"{\"type\":\"cloud:playtime:got\",\"total_seconds\":" +
                     std::to_wstring(total) + L"}");
        }
    }
    else if (type == "cloud:playtime:upload") {
        std::wstring pid = util::StringToWString(ExtractString(json, "player_id"));
        long long session = ExtractInt(json, "session_seconds");
        long long clientTotal = ExtractInt(json, "client_total_seconds");
        long long srv = cloud_->UploadPlaytime(pid, session, clientTotal);
        if (srv >= 0) {
            SendToJs(L"{\"type\":\"cloud:playtime:uploaded\",\"total_seconds\":" +
                     std::to_wstring(srv) + L"}");
        }
    }
}

// ---------------- OAuth 登录 ----------------
void App::DoOAuthLogin() {
    OAuth oauth(cfg_);
    webview_.Navigate(oauth.BuildAuthorizeUrl());
}

void App::OnNavigate(const std::wstring& uri) {
    ParseCallback(uri);
}

void App::ParseCallback(const std::wstring& uri) {
    // 先处理授权失败：LittleSkin 会在回调 URL 中带回 error / error_description
    std::wstring err = GetQueryParam(uri, L"error");
    if (!err.empty()) {
        std::wstring desc = GetQueryParam(uri, L"error_description");
        std::wstring reason = desc.empty() ? (L"OAuth 错误: " + err) : desc;
        SendToJs(L"{\"type\":\"oauth:fail\",\"reason\":\"" + JsonEscape(reason) + L"\"}");
        // 回到启动器首页，避免停留在空白/错误页；把错误原因交给页面用原生 toast 显示
        webview_.Reload(reason);
        return;
    }

    // uri 形如 stardust://oauth/callback?code=xxxx
    size_t q = uri.find(L"?code=");
    if (q == std::wstring::npos) return;
    std::wstring code = uri.substr(q + 6);
    // 去掉可能的后续参数
    size_t amp = code.find(L'&');
    if (amp != std::wstring::npos) code = code.substr(0, amp);

    OAuth oauth(cfg_);
    if (oauth.ExchangeCode(code, cfg_)) {
        cfg_.Save();
        loggedIn_ = true;
        currentPlayer_ = cfg_.currentRole.empty() ? L"Steve_Chan" : cfg_.currentRole;
        pendingLoginSuccess_ = true;
        pendingUsername_ = currentPlayer_;
        // 重新渲染启动器首页（内嵌 HTML，NavigationCompleted 里会调用 loginSuccess 切到已登录态）
        webview_.Reload();
    } else {
        SendToJs(L"{\"type\":\"oauth:fail\",\"reason\":\"token 换取失败\"}");
    }
}

// ---------------- 游戏启动 ----------------
void App::OnJsLaunchGame() {
    if (playing_) return;
    if (cfg_.selectedJava.empty() || cfg_.selectedCore.empty()) {
        SendToJs(L"{\"type\":\"game:error\",\"reason\":\"请先在设置里选择 Java 和游戏核心\"}");
        return;
    }
    // 找到选中核心的 jar
    std::wstring jar;
    for (auto& c : cfg_.coreList) {
        if (c.dir == cfg_.selectedCore) { jar = c.jar; break; }
    }
    if (jar.empty()) {
        SendToJs(L"{\"type\":\"game:error\",\"reason\":\"未找到核心 jar 文件\"}");
        return;
    }

    game_.SetOnExit([this]() { OnGameExit(); });
    if (!game_.Start(cfg_.selectedJava, jar, cfg_.memoryMb, cfg_.gameDir)) return;

    playing_ = true;
    gameStartMs_ = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    SendToJs(L"{\"type\":\"game:started\"}");

    // 启动计时推送线程
    tickThread_ = std::thread([this]() { GameTickLoop(); });
}

void App::GameTickLoop() {
    while (playing_) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!playing_) break;
        long long now = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        long long elapsed = now - gameStartMs_;
        SendToJs(L"{\"type\":\"game:tick\",\"ms\":" + std::to_wstring(elapsed) + L"}");
    }
}

void App::OnGameExit() {
    if (!playing_) return;
    playing_ = false;
    if (tickThread_.joinable()) tickThread_.join();

    long long now = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    long long sessionMs = now - gameStartMs_;
    long long sessionSec = sessionMs / 1000;

    // 上传云端（按用户要求暂不启用，仅本地记录时长）
    if (CLOUD_SYNC_ENABLED && cloud_ && loggedIn_) {
        long long clientTotal = sessionSec; // 本地累计可在此基础上累加，这里简化传输本次
        cloud_->UploadPlaytime(currentPlayer_, sessionSec, clientTotal);
    }

    SendToJs(L"{\"type\":\"game:stopped\"}");
}

// ---------------- 回传列表给前端 ----------------
void App::SendJavaList() {
    std::wstring jl = L"{\"type\":\"sys:java\",\"list\":[";
    for (size_t i = 0; i < cfg_.javaList.size(); ++i) {
        if (i) jl += L",";
        jl += L"\"" + JsonEscape(cfg_.javaList[i].display) + L"\"";
    }
    jl += L"]}";
    SendToJs(jl);
}

void App::SendCoreList() {
    std::wstring cl = L"{\"type\":\"sys:cores\",\"list\":[";
    for (size_t i = 0; i < cfg_.coreList.size(); ++i) {
        if (i) cl += L",";
        cl += L"\"" + JsonEscape(cfg_.coreList[i].name) + L"\"";
    }
    cl += L"]}";
    SendToJs(cl);
}

// ---------------- 选择 Java 文件夹 ----------------
void App::SelectJavaFolder() {
    IFileDialog* pfd = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&pfd)))) return;
    DWORD opts = 0;
    pfd->GetOptions(&opts);
    pfd->SetOptions(opts | FOS_PICKFOLDERS);
    if (SUCCEEDED(pfd->Show(hwnd_))) {
        IShellItem* psi = nullptr;
        if (SUCCEEDED(pfd->GetResult(&psi))) {
            wchar_t* path = nullptr;
            if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                std::wstring binDir(path);
                std::wstring exe = JavaScanner::PickManual(binDir, cfg_);
                if (!exe.empty()) {
                    cfg_.Save();
                    SendJavaList();   // 回传更新后的 Java 列表
                } else {
                    SendToJs(L"{\"type\":\"game:error\",\"reason\":\"该目录未找到 java.exe\"}");
                }
                CoTaskMemFree(path);
            }
            psi->Release();
        }
    }
    pfd->Release();
}
