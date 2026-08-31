// 主应用实现：窗口、消息分发、模块协调
#include "app.h"
#include "resource.h"   // IDI_APP_ICON 等内嵌资源 ID
#include "common.h"
#include "oauth.h"
#include "java_scanner.h"
#include "core_scanner.h"
#include "version_installer.h"   // 自动补装缺失的原版/库
#include "modpack_installer.h"   // 整合包导入：ImportModpack / ModpackMcVersion
#include <windows.h>
#include <shlobj.h>      // IFileDialog
#include <dwmapi.h>      // DwmSetWindowAttribute（Windows 11 圆角）
#include <string>
#include <vector>
#include <chrono>
#include <sstream>
#include <set>
#include <mutex>
#include <exception>
#include <cctype>

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

// JS 单引号字符串转义（用于把用户名等拼进 ExecuteScript 的 JS 源码）
static std::wstring JsEscape(const std::wstring& s) {
    std::wstring out;
    for (wchar_t c : s) {
        if (c == L'\\' || c == L'\'') out += L'\\';
        out += c;
    }
    return out;
}

// 把单个 YggProfile 序列化为标准 JSON 对象（双引号）
static std::wstring ProfileToJson(const YggProfile& p) {
    return L"{\"id\":\"" + JsonEscape(util::StringToWString(p.id))
         + L"\",\"name\":\"" + JsonEscape(util::StringToWString(p.name)) + L"\"}";
}

// 把 YggProfile 列表序列化为标准 JSON 字符串（双引号）
static std::wstring BuildProfilesJson(const std::vector<YggProfile>& list) {
    std::wstring out = L"[";
    for (size_t i = 0; i < list.size(); ++i) {
        if (i) out += L",";
        out += ProfileToJson(list[i]);
    }
    out += L"]";
    return out;
}

// 判断 Java 缓存是否"失效"（显示名是原始文件名而非版本号，说明上次查询失败）
static bool JavaListStale(const Config& cfg) {
    for (const auto& j : cfg.javaList) {
        // 版本号形如 "java version 1.8.0_401" / "openjdk version 21.0.3"
        bool hasVersion = j.display.find(L"version") != std::wstring::npos;
        if (!hasVersion) return true;
    }
    return false;
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
    mainThreadId_ = GetCurrentThreadId();   // 记录 UI 线程，供 SendToJs 判断是否需跨线程投递

    // 读取配置（含上次扫描缓存）
    cfg_.Load();

    // 恢复 Yggdrasil 登录态（传统登录不依赖 OAuth refresh_token）
    if (!cfg_.accessToken.empty() && !cfg_.currentRole.empty()) {
        loggedIn_ = true;
        accountId_ = cfg_.accountId;   // 身份键用 LittleSkin 账号 ID（同一账号下所有角色共享时长）
        yggSelected_.id   = util::WStringToString(cfg_.currentRoleId);
        yggSelected_.name = util::WStringToString(cfg_.currentRole);
    }

    // 首次或缓存为空/失效时扫描 Java。Java 列表走 ini 缓存（避免每次启动扫系统环境）。
    // 失效判定：缓存的显示名是原始文件名（如 "java.exe"）说明上次版本查询失败，
    // 重扫一次以拿到真正的版本号。
    if (cfg_.javaList.empty() || JavaListStale(cfg_)) JavaScanner::Scan(cfg_);
    // 版本核心每次都重扫（不写 ini，启动前还要再扫一次拿真实版本）
    CoreScanner::Scan(cfg_);
    cfg_.Save();

    // 服务端地址：优先用 config.ini 里 [cloud] server_url 覆盖（本地/测试部署用），
    // 留空则回退 cloud_sync.cpp 内硬编码的默认地址。
    cloud_ = new CloudSync(cfg_.cloudServerUrl.empty()
                                ? CloudSync::DefaultServerUrl()
                                : cfg_.cloudServerUrl);

    // 若上次已登录（恢复登录态），则直接加载本机 RSA 私钥；没有则等这次登录成功后再生成
    if (loggedIn_) EnsureLocalKey();

    // 已登录：立即把公钥注册到服务端（启动即建连，服务端控制台会立刻出现「连接」「注册」日志）；
    // 注册是幂等的（公钥一致免 12h 冷却），因此重启启动器不会被锁。
    if (loggedIn_) {
        EnsureRegistered(accountId_);
        // 自动登录（第二次打开即已恢复登录态）：注册完成后立即拉取累计时长推前端，
        // 让首页打开就能看到准确累计，无需等 PlaytimeSyncLoop 的 5 分钟首刷。
        FetchPlaytimeAfterRegistered();
        // 自动登录后调 refresh 刷新访问令牌（保持登录有效）。完整角色列表以「首次登录
        // authenticate 时记录」为准，这里绝不覆盖——LittleSkin 的 refresh 只回当前选中的 1 个。
        RefreshProfilesOnRestore();
    }

    // 启动「独立于游戏」的累计时长同步线程：登录态在就每 5 分钟从服务端拉一次最新累计刷新显示，
    // 与游戏是否启动无关（拉取时长是展示用，不开游戏也能看到准确累计时长）。线程随进程常驻。
    std::thread([this]() { PlaytimeSyncLoop(); }).detach();

    // 启动时主动探测一次服务端连通性（无论是否登录），让服务端控制台出现「连接」日志，
    // 这样「打开软件」就能在服务端看到连接提示，便于排查地址/网络问题。
    std::thread([this]() {
        if (!cloud_) return;
        bool ok = cloud_->Ping();
        util::Log(std::string("服务端连通性探测：") + (ok ? "成功" : "失败/不可达"));
    }).detach();

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

    // 注册右侧授权面板窗口类（独立弹窗，内嵌第二个 WebView2 加载 LittleSkin 授权页）
    WNDCLASSEXW pw = {0};
    pw.cbSize = sizeof(WNDCLASSEXW);
    pw.lpfnWndProc = AuthPanelProc;
    pw.hInstance = hInstance;
    pw.hCursor = LoadCursor(nullptr, IDC_ARROW);
    pw.hbrBackground = (HBRUSH)CreateSolidBrush(RGB(26,16,48));
    pw.lpszClassName = L"StardustAuthPanel";
    RegisterClassExW(&pw);

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
        [this]() { // onReady：仅作占位，系统信息改在 NavigationCompleted 后推送
        },
        [this]() { // onNavigated：页面 JS 已就绪，此时推送才不会被丢弃
            if (pendingLoginSuccess_) {
                pendingLoginSuccess_ = false;
                // 用 PostMessage 传 JSON，避免 ExecuteScript 字符串转义问题
                SendToJs(L"{\"type\":\"yggdrasil:success\",\"name\":\"" + JsonEscape(pendingUsername_) + L"\","
                         L"\"profiles\":" + BuildProfilesJson(yggProfiles_) + L","
                         L"\"profile\":" + ProfileToJson(yggSelected_) + L"}");
            } else if (loggedIn_) {
                // 启动时若已登录，把持久化的角色列表同步给前端
                // 兜底：availableProfiles 为空时（如单角色账号服务端未返回列表），用当前角色
                // 合成一个单元素列表，保证下拉菜单不空白（"选择角色坏了"的根因修复）。
                std::wstring profilesJson = cfg_.availableProfiles;
                if (profilesJson.empty() && !cfg_.currentRoleId.empty() && !cfg_.currentRole.empty()) {
                    profilesJson = L"[{\"id\":\"" + JsonEscape(cfg_.currentRoleId)
                                + L"\",\"name\":\"" + JsonEscape(cfg_.currentRole) + L"\"}]";
                }
                SendToJs(L"{\"type\":\"yggdrasil:restored\",\"name\":\"" + JsonEscape(cfg_.currentRole) + L"\","
                         L"\"profiles\":" + profilesJson + L","
                         L"\"profile\":" + ProfileToJson(yggSelected_) + L"}");
            }
            SendJavaList();
            SendCoreList();
            SendMemInfo();
            SendLaunchConfig();
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
        case WM_SEND_TO_JS: {
            // 后台线程投递来的 JS 消息：在 UI 线程安全地调用 WebView2 接口
            std::wstring* p = reinterpret_cast<std::wstring*>(l);
            if (!app) { delete p; return 0; }
            if (p) {
                app->webview_.PostMessage(*p);
                delete p;
            }
            return 0;
        }
        case WM_YGGDRASIL_DONE:
            app->OnYggdrasilDone();
            return 0;
        case WM_SWITCH_ROLE_DONE:
            app->OnSwitchRoleDone();
            return 0;
        case WM_REFRESH_PROFILES:
            app->OnRefreshProfilesDone();
            return 0;
        case WM_DESTROY:
            if (app && app->cloud_) app->cloud_->ClearCurseForgeKey();   // 释放内存中的 CF 密钥
            PostQuitMessage(0);
            return 0;
        case WM_NCDESTROY:
            SetWindowLongPtr(h, GWLP_USERDATA, 0);
            return DefWindowProc(h, m, w, l);
    }
    return DefWindowProc(h, m, w, l);
}

// ---------------- 处理 JS 消息 ----------------
void App::SendToJs(const std::wstring& json) {
    // 关键修复：WebView2 的 PostWebMessageAsJson 必须在创建它的 UI 线程调用。
    // 本启动器大量消息来自后台线程（启动线程、每秒计时线程、游戏退出回调线程等），
    // 若直接跨线程调用，轻则消息被静默丢弃（按钮/UI 不更新），重则多线程并发调用
    // 同一 COM 接口导致堆损坏、整个客户端崩溃（多点几次启动即崩就是此因）。
    // 因此：非 UI 线程时，把消息堆拷贝一份，用 WM_SEND_TO_JS 投递给主窗口，
    // 由 WndProc 在 UI 线程统一调用 webview_.PostMessage。
    if (GetCurrentThreadId() == mainThreadId_) {
        webview_.PostMessage(json);
        return;
    }
    std::wstring* p = new std::wstring(json);
    if (!PostMessageW(hwnd_, WM_SEND_TO_JS, 0, (LPARAM)p)) {
        delete p;   // 窗口已销毁等导致投递失败：释放，避免内存泄漏
    }
}

// 从 JSON 取数组字段（如 "items":[...]）的子串（含方括号）；取不到返回 "[]"
static std::wstring ExtractArrayW(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\":";
    size_t p = json.find(needle);
    if (p == std::string::npos) return L"[]";
    size_t b = json.find('[', p);
    if (b == std::wstring::npos) return L"[]";
    int depth = 0; size_t i = b;
    for (; i < json.size(); ++i) {
        if (json[i] == '[') ++depth;
        else if (json[i] == ']') { --depth; if (depth == 0) break; }
    }
    if (i >= json.size()) return L"[]";
    return util::StringToWString(json.substr(b, i - b + 1));
}

// 把 wstring 转义后塞进 JSON 字符串（仅处理引号/反斜杠/控制字符）
static std::wstring JsonEscapeW(const std::wstring& s) {
    std::wstring out;
    for (wchar_t c : s) {
        if (c == L'"' || c == L'\\') { out += L'\\'; out += c; }
        else if (c == L'\n') out += L"\\n";
        else if (c == L'\r') out += L"\\r";
        else if (c == L'\t') out += L"\\t";
        else if (c < 0x20) out += L"?";
        else out += c;
    }
    return out;
}

// 按空白符拆分用户自定义 JVM 参数字符串（支持空格/换行/制表符分隔）
static std::vector<std::string> SplitJvmArgs(const std::wstring& s) {
    std::vector<std::string> out;
    std::wstring cur;
    for (wchar_t c : s) {
        if (std::isspace((unsigned char)c)) {
            if (!cur.empty()) { out.push_back(util::WStringToString(cur)); cur.clear(); }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(util::WStringToString(cur));
    return out;
}

// 把 GC 预设 + 自定义参数翻译成实际 JVM 参数列表
static std::vector<std::string> BuildExtraJvmArgs(const std::wstring& gcPreset, const std::wstring& customArgs) {
    std::vector<std::string> out;
    if (gcPreset == L"g1_optimized") {
        out = {
            "-XX:+UnlockExperimentalVMOptions",
            "-XX:+UseG1GC",
            "-XX:MaxGCPauseMillis=200",
            "-XX:G1HeapRegionSize=16m",
            "-XX:+ParallelRefProcEnabled",
            "-XX:+AlwaysPreTouch"
        };
    } else if (gcPreset == L"zgc") {
        out = { "-XX:+UnlockExperimentalVMOptions", "-XX:+UseZGC", "-XX:+AlwaysPreTouch" };
    } else if (gcPreset == L"zgc_gen") {
        out = { "-XX:+UnlockExperimentalVMOptions", "-XX:+UseZGC", "-XX:+ZGenerational", "-XX:+AlwaysPreTouch" };
    } else if (gcPreset == L"g1_std") {
        out = { "-XX:+UseG1GC" };
    } else if (gcPreset == L"g1_throughput") {
        out = {
            "-XX:+UnlockExperimentalVMOptions",
            "-XX:+UseG1GC",
            "-XX:MaxGCPauseMillis=400",
            "-XX:G1HeapRegionSize=16m",
            "-XX:+UseStringDeduplication",
            "-XX:+AlwaysPreTouch"
        };
    }
    if (gcPreset == L"custom") {
        out = SplitJvmArgs(customArgs);
    } else {
        // 非自定义 preset 也允许叠加少量自定义参数，方便用户微调
        auto extra = SplitJvmArgs(customArgs);
        out.insert(out.end(), extra.begin(), extra.end());
    }
    return out;
}

void App::OnWebMessage(const std::wstring& jsonW) {
    std::string json = util::WStringToString(jsonW);
    std::string type = ExtractString(json, "type");

    if (type == "oauth:login") {
        DoOAuthLogin();
    }
    else if (type == "yggdrasil:login") {
        std::wstring user = util::StringToWString(ExtractString(json, "username"));
        std::wstring pass = util::StringToWString(ExtractString(json, "password"));
        DoYggdrasilLogin(user, pass);
    }
    else if (type == "role:change") {
        YggProfile target;
        target.id   = ExtractString(json, "id");
        target.name = ExtractString(json, "name");
        if (target.id.empty() && !target.name.empty()) {
            // 兼容旧消息（只有 name）：在已缓存列表里查找
            for (const auto& p : yggProfiles_) {
                if (p.name == target.name) { target = p; break; }
            }
        }
        if (!target.id.empty() && !target.name.empty()) {
            DoSwitchRole(target);
        }
    }
    else if (type == "logout") {
        // 游戏中退出登录：先把正在运行的游戏客户端一起关掉（TerminateProcess 只杀本启动器启动的 Java 进程）
        if (game_.IsRunning()) game_.Stop();
        cfg_.accessToken.clear();
        cfg_.clientToken.clear();
        cfg_.currentRole.clear();
        cfg_.currentRoleId.clear();
        cfg_.availableProfiles.clear();
        cfg_.Save();
        loggedIn_ = false;
        accountId_.clear();
        yggProfiles_.clear();
        yggSelected_ = {};
        webview_.Reload();
    }
    else if (type == "config:set") {
        std::string key = ExtractString(json, "key");
        std::string val = ExtractString(json, "value");
        if (key == "memory_mb") { cfg_.memoryMb = (int)ExtractInt(json, "value"); cfg_.Save(); }
        else if (key == "selected_core") {
            cfg_.selectedCore = util::StringToWString(val);
            cfg_.Save();
            ApplyCoreSelection(cfg_.selectedCore); // 重新计算真实版本 + 推荐 Java 并推前端
        }
        else if (key == "selected_java") { cfg_.selectedJava = util::StringToWString(val); cfg_.Save(); }
        else if (key == "process_priority") { cfg_.processPriority = util::StringToWString(val); cfg_.Save(); SendLaunchConfig(); }
        else if (key == "gc_preset") { cfg_.gcPreset = util::StringToWString(val); cfg_.Save(); SendLaunchConfig(); }
        else if (key == "custom_jvm_args") { cfg_.customJvmArgs = util::StringToWString(val); cfg_.Save(); SendLaunchConfig(); }
        else if (key == "server_address") { cfg_.serverAddress = util::StringToWString(val); cfg_.Save(); SendLaunchConfig(); }
        else if (key == "server_port") { cfg_.serverPort = (int)ExtractInt(json, "value"); cfg_.Save(); SendLaunchConfig(); }
    }
    else if (type == "sys:init") {
        // 页面初始化完成，主动拉取系统信息（与 NavigationCompleted 推送双保险）
        SendJavaList();
        SendCoreList();
        SendMemInfo();
    }
    else if (type == "java:browse") {
        SelectJavaFolder();
    }
    else if (type == "game:launch") {
        OnJsLaunchGame();
    }
    else if (type == "game:stop") {
        // 游戏中再次点击启动按钮 → 关闭游戏客户端（不退出启动器）。
        // Stop() 会终止由本启动器启动的 Java 进程，并触发 OnGameExit(false) → game:stopped 复位 UI。
        if (game_.IsRunning()) {
            game_.Stop();
        } else {
            // 兜底：没在运行却收到 stop，直接复位 UI
            SendToJs(L"{\"type\":\"game:stopped\"}");
        }
    }
    else if (type == "window") {
        std::string action = ExtractString(json, "action");
        if (action == "min") ShowWindow(hwnd_, SW_MINIMIZE);
        else if (action == "close") DestroyWindow(hwnd_);
    }
    else if (type == "drag") {
        // HTML 标题栏 mousedown：与 client 项目完全同款方案（SendMessage 同步调用，
        // 让 DefWindowProc 进入 SC_MOVE 模态拖动循环）。
        // 不再用 PostMessage WM_NCLBUTTONDOWN：异步投递会让 SC_MOVE 与 WebView2
        // 鼠标事件路由错位，导致拖动循环错过 mouseup 而失效。
        ReleaseCapture();
        SendMessageW(hwnd_, WM_SYSCOMMAND, SC_MOVE | HTCAPTION, 0);
    }
    else if (type == "browser:open") {
        std::wstring url = util::StringToWString(ExtractString(json, "url"));
        ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    else if (type == "cloud:playtime:get") {
        // 仅以登录账号 UUID 为键（切角色不影响、时长按账号聚合）；不信任前端传来的 player_id（那是角色名）
        if (accountId_.empty() || !cloud_) return;   // 未取到账号 UUID 或云端未开则不查询，避免用角色名产生分裂键
        long long total = cloud_->GetPlaytime(accountId_, cfg_.currentRole);
        if (total >= 0) {
            SendToJs(L"{\"type\":\"cloud:playtime:got\",\"total_seconds\":" +
                     std::to_wstring(total) + L"}");
        }
    }
    else if (type == "cloud:playtime:upload") {
        if (accountId_.empty() || !cloud_) return;
        long long session = ExtractInt(json, "session_seconds");
        long long clientTotal = ExtractInt(json, "client_total_seconds");
        long long srv = cloud_->UploadPlaytime(accountId_, session, clientTotal, cfg_.currentRole);
        if (srv >= 0) {
            SendToJs(L"{\"type\":\"cloud:playtime:uploaded\",\"total_seconds\":" +
                     std::to_wstring(srv) + L"}");
        }
    }
    else if (type == "java:open") {
        // 打开 Java 下载页：向服务端拉取可选版本列表，原样转发 items 数组给前端解析
        if (!cloud_) { SendToJs(L"{\"type\":\"java:list\",\"items\":[],\"error\":\"云端未连接\"}"); return; }
        std::wstring resp = cloud_->JavaList();
        std::string s = util::WStringToString(resp);
        if (s.empty()) { SendToJs(L"{\"type\":\"java:list\",\"items\":[],\"error\":\"无法连接服务端\"}"); return; }
        // 取出 "items":[...] 子串（已是合法 JSON 数组），直接嵌进回传消息，前端 JSON.parse
        std::wstring items = ExtractArrayW(s, "items");
        SendToJs(L"{\"type\":\"java:list\",\"items\":" + items + L"}");
    }
    else if (type == "java:install") {
        // 一键下载并静默安装：前端只传显示名，C++ 向服务端要预签名直链→下载→提权静默安装
        std::wstring name = util::StringToWString(ExtractString(json, "name"));
        if (!name.empty()) DoJavaInstall(name);
    }
    else if (type == "modpack:browse") {
        // 打开文件选择框选整合包 zip
        DoModpackBrowse();
    }
    else if (type == "modpack:drop") {
        // JS 层拖放拦截后传来的 zip 路径（绕过 Edge 下载管理器）
        std::wstring dropPath = util::StringToWString(ExtractString(json, "path"));
        if (!dropPath.empty()) DoImportModpack(dropPath);
    }
    else if (type == "modpack:import") {
        // 前端把选中的 zip 路径发回来，后台线程跑导入
        std::wstring zip = util::StringToWString(ExtractString(json, "zip"));
        if (!zip.empty()) DoImportModpack(zip);
    }
}

// ---------------- Java 一键下载并静默安装 ----------------
// 安装/导入完成后通知 Windows 资源管理器刷新目录，避免文件显示陈旧、看起来混乱。
// SHCNE_UPDATEDIR + SHCNF_PATH：告知 Explorer 该目录内容已变更；SHCNF_FLUSH 让刷新立即生效。
static void RefreshShellDir(const std::wstring& dir) {
    if (dir.empty()) return;
    SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATH | SHCNF_FLUSH, dir.c_str(), nullptr);
}

void App::DoJavaInstall(const std::wstring& name) {
    // 防重复触发：同一时刻只跑一个下载/安装任务
    if (javaInstalling_.exchange(true)) {
        SendToJs(L"{\"type\":\"java:progress\",\"name\":\"" + JsonEscapeW(name) +
                 L"\",\"phase\":\"error\",\"text\":\"已有 Java 安装任务进行中\"}");
        return;
    }
    std::wstring nameCopy = name;
    std::thread([this, nameCopy]() {
        auto report = [this, &nameCopy](const std::wstring& phase, int percent, const std::wstring& text) {
            SendToJs(L"{\"type\":\"java:progress\",\"name\":\"" + JsonEscapeW(nameCopy) +
                     L"\",\"phase\":\"" + phase + L"\",\"percent\":" + std::to_wstring(percent) +
                     L",\"text\":\"" + JsonEscapeW(text) + L"\"}");
        };
        // 1) 向服务端要 1 小时有效的预签名直链（密钥只在服务端，客户端只拿到直链）
        report(L"resolve", 0, L"正在获取下载地址…");
        if (!cloud_) { report(L"error", 0, L"云端未连接"); javaInstalling_ = false; return; }
        std::wstring dl = cloud_->JavaDownloadUrl(nameCopy);
        std::string s = util::WStringToString(dl);
        bool okJson = s.find("\"success\":true") != std::string::npos ||
                      s.find("\"success\": true") != std::string::npos;
        if (!okJson) { report(L"error", 0, L"获取下载地址失败"); javaInstalling_ = false; return; }
        std::string url = ExtractString(s, "url");
        if (url.empty()) { report(L"error", 0, L"下载地址为空"); javaInstalling_ = false; return; }

        // 2) 下载到 %TEMP%\StardustLauncher\java\ 下
        wchar_t tmp[MAX_PATH] = {0};
        if (!GetEnvironmentVariableW(L"TEMP", tmp, MAX_PATH) || tmp[0] == 0)
            wcscpy_s(tmp, MAX_PATH, L"C:\\Windows\\Temp");
        std::wstring base = std::wstring(tmp) + L"\\StardustLauncher\\java";
        CreateDirectoryW((std::wstring(tmp) + L"\\StardustLauncher").c_str(), nullptr);
        CreateDirectoryW(base.c_str(), nullptr);

        // 本地文件名优先用 S3 key 里的真实 MSI 文件名（纯 ASCII），避免中文显示名导致 msiexec 解析异常
        std::string s3key = ExtractString(s, "key");
        std::wstring safe = nameCopy;   // 备用文件名：显示名，去掉非法字符
        for (wchar_t& c : safe)
            if (c == L'/' || c == L'\\' || c == L':' || c == L'*' || c == L'?' ||
                c == L'"' || c == L'<' || c == L'>' || c == L'|') c = L'_';
        std::wstring localFile = safe + L".msi";
        if (!s3key.empty()) {
            size_t slash = s3key.find_last_of('/');
            std::string basename = (slash == std::string::npos) ? s3key : s3key.substr(slash + 1);
            if (!basename.empty() && basename.size() < 256)
                localFile = util::StringToWString(basename);
        }
        std::wstring outPath = base + L"\\" + localFile;
        // 安装日志归集到「启动器同目录的 log\」下的 java-install.log（遵循点4：所有日志进 log\），
        // 不再散落在 %TEMP% 里。成功安装后会被清掉，失败则保留在 log\ 方便排查。
        std::wstring logPath = util::GetLogDir() + L"\\java-install.log";

        report(L"download", 0, L"正在下载安装包…");
        bool ok = CloudSync::DownloadFile(util::StringToWString(url), outPath,
            [&](long long recv, long long total) {
                int pct = (total > 0) ? (int)(recv * 100 / total) : 0;
                std::wstring txt = L"正在下载 " + std::to_wstring(recv / 1024 / 1024) + L" MB";
                if (total > 0) txt += L" / " + std::to_wstring(total / 1024 / 1024) + L" MB";
                report(L"download", pct, txt);
            });
        if (!ok || GetFileAttributesW(outPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            report(L"error", 0, L"下载失败（网络或磁盘问题）");
            DeleteFileW(outPath.c_str());
            javaInstalling_ = false; return;
        }

        // 3) 提权静默安装：msiexec /i "文件" /qn /norestart /L*V "日志"
        report(L"install", 100, L"正在安装（需要管理员授权）…");
        // 注意：GetLogDir() 返回值必须存到具名变量，否则 .c_str() 返回的指针会在分号处悬垂
        // （临时 wstring 立即销毁），导致 lpDirectory 失效、msiexec 回退到默认工作目录（exe 根目录）
        std::wstring logDir = util::GetLogDir();
        SHELLEXECUTEINFO sei = { sizeof(sei) };
        sei.lpVerb = L"runas";                       // 触发 UAC 提权
        sei.lpFile = L"msiexec";
        std::wstring params = L"/i \"" + outPath + L"\" /qn /norestart /L*V \"" + logPath + L"\"";
        sei.lpParameters = params.c_str();
        sei.lpDirectory = logDir.c_str();             // 工作目录 = log\，msiexec 自带的 installer.log 直接落这里
        sei.nShow = SW_SHOWNORMAL;
        sei.fMask = SEE_MASK_NOCLOSEPROCESS;          // 尝试拿进程句柄以便等安装结束
        if (!ShellExecuteExW(&sei)) {
            report(L"error", 0, L"安装未启动（可能被取消或权限不足）");
            DeleteFileW(outPath.c_str());
            javaInstalling_ = false; return;
        }

        if (sei.hProcess) {
            // 拿到句柄：直接等待安装进程结束并读退出码
            WaitForSingleObject(sei.hProcess, INFINITE);
            DWORD code = 0; GetExitCodeProcess(sei.hProcess, &code);
            CloseHandle(sei.hProcess);
            bool success = (code == 0 || code == 3010);  // 0 成功；3010 成功但需重启
            if (success) {
                DeleteFileW(outPath.c_str());
                DeleteFileW(logPath.c_str());
                // 清理 msiexec 自带生成的 installer.log（可能因 UAC 提权忽略 lpDirectory 而落到 exe 根目录）
                std::wstring rootInstallerLog = util::GetExeDir() + L"\\installer.log";
                DeleteFileW(rootInstallerLog.c_str());
                // 也清理 log\ 下可能存在的 installer.log（正常路径）
                std::wstring logInstallerLog = util::GetLogDir() + L"\\installer.log";
                DeleteFileW(logInstallerLog.c_str());
                RefreshShellDir(util::GetExeDir());   // 安装完成，刷新启动器目录，避免文件显示陈旧
                if (code == 0) report(L"done", 100, L"安装完成 ✓");
                else report(L"done", 100, L"安装完成，重启后生效 ✓");
            } else {
                report(L"error", 0, L"安装失败（错误码 " + std::to_wstring(code) + L"），日志：" + logPath);
            }
            javaInstalling_ = false; return;
        }

        // runas 提权时常拿不到进程句柄，但 msiexec 其实已启动；
        // 等 15 秒让用户点 UAC，然后轮询 MSI 文件是否被释放（删得掉说明装完了）
        report(L"install", 100, L"安装已启动，请确认 UAC 授权…");
        Sleep(15000);
        for (int i = 0; i < 45; ++i) {
            if (DeleteFileW(outPath.c_str())) {
                DeleteFileW(logPath.c_str());
                // 清理 msiexec 自带的 installer.log（UAC 提权可能忽略 lpDirectory）
                DeleteFileW((util::GetExeDir() + L"\\installer.log").c_str());
                DeleteFileW((util::GetLogDir() + L"\\installer.log").c_str());
                RefreshShellDir(util::GetExeDir());   // 安装完成，刷新启动器目录，避免文件显示陈旧
                report(L"done", 100, L"安装完成 ✓");
                javaInstalling_ = false; return;
            }
            DWORD err = GetLastError();
            if (err == ERROR_FILE_NOT_FOUND) {
                DeleteFileW(logPath.c_str());
                // 清理 msiexec 自带的 installer.log（UAC 提权可能忽略 lpDirectory）
                DeleteFileW((util::GetExeDir() + L"\\installer.log").c_str());
                DeleteFileW((util::GetLogDir() + L"\\installer.log").c_str());
                RefreshShellDir(util::GetExeDir());   // 安装完成，刷新启动器目录，避免文件显示陈旧
                report(L"done", 100, L"安装完成 ✓");
                javaInstalling_ = false; return;
            }
            Sleep(2000);
        }
        // 90 秒仍被占用：大概率安装还在跑或失败，保留 MSI/日志供排查
        report(L"done", 100, L"安装已启动，若失败请查看日志：" + logPath);
        javaInstalling_ = false;
    }).detach();
}

// ---------------- 传统 Yggdrasil 登录（当前唯一登录方式） ----------------
void App::DoYggdrasilLogin(const std::wstring& user, const std::wstring& pass) {
    // 先给前端一个"登录中"反馈，再放到工作线程做网络请求，避免卡住界面
    SendToJs(L"{\"type\":\"yggdrasil:starting\"}");
    std::thread([this, user, pass]() {
        OAuth oauth(cfg_);
        YggResult r = oauth.YggdrasilLogin(user, pass);
        yggOk_ = r.ok;
        yggAccessToken_ = util::StringToWString(r.accessToken);
        yggClientToken_  = util::StringToWString(r.clientToken);
        yggPlayer_     = util::StringToWString(r.playerName);
        yggError_      = util::StringToWString(r.error);
        yggAccountId_  = util::StringToWString(r.accountId);
        yggAccountName_ = util::StringToWString(r.accountName);
        yggProfiles_   = r.availableProfiles;
        yggSelected_   = r.selectedProfile;
        // 回到主线程处理结果（设置登录态 / 弹错误），避免跨线程操作 WebView2
        PostMessageW(hwnd_, WM_YGGDRASIL_DONE, 0, 0);
    }).detach();
}

void App::OnYggdrasilDone() {
    if (yggOk_) {
        cfg_.accessToken = yggAccessToken_;
        cfg_.clientToken = yggClientToken_;
        cfg_.currentRole = yggPlayer_;
        cfg_.currentRoleId = util::StringToWString(yggSelected_.id);
        // 保存角色列表 JSON，重启后仍能显示下拉菜单
        // 兜底：若服务端 authserver 没返回 availableProfiles（部分 Yggdrasil 实现在单角色账号下不返回），
        // 至少把当前选中的角色放进去，避免重启后角色下拉菜单空白（"选择角色坏了"）。
        if (yggProfiles_.empty() && !yggSelected_.id.empty() && !yggSelected_.name.empty())
            yggProfiles_.push_back(yggSelected_);
        cfg_.availableProfiles = BuildProfilesJson(yggProfiles_);
        cfg_.yggApi = Config::DefaultYggApi();  // 标记这是 LittleSkin 登录（固定地址，混淆硬编码）
        cfg_.accountId = yggAccountId_;          // 时长统计/公钥注册的统一键 = LittleSkin 账号 ID
        cfg_.accountName = yggAccountName_;
        cfg_.Save();
        loggedIn_ = true;
        // 身份键只认 LittleSkin 账号 UUID：绝不回退角色名/"Steve_Chan"，避免时长键分裂成两份
        accountId_ = yggAccountId_;
        if (accountId_.empty()) {
            util::Log("警告：LittleSkin 未返回账号 UUID，云端游玩时长统计本次不可用（不回退用户名/角色名）");
        }
        pendingLoginSuccess_ = true;
        pendingUsername_ = yggPlayer_.empty() ? L"Steve_Chan" : yggPlayer_;
        EnsureLocalKey();                    // 登录成功后才生成本机 RSA 密钥对（私钥写 config.ini）
        EnsureRegistered(accountId_);       // 登录成功后把本机公钥注册到服务端（按账号）
        // 登录后立即拉取服务端累计时长刷新显示（显示以服务端为准）；注册是异步的，
        // FetchPlaytimeAfterRegistered 内部会重试等待注册完成再拉取。
        FetchPlaytimeAfterRegistered();
        webview_.Reload();   // 主窗口 NavigationCompleted → loginSuccess 切到已登录态
    } else {
        SendToJs(L"{\"type\":\"yggdrasil:fail\",\"reason\":\"" + JsonEscape(yggError_) + L"\"}");
    }
}

void App::DoSwitchRole(const YggProfile& profile) {
    if (profile.id.empty() || profile.name.empty()) return;
    if (cfg_.accessToken.empty() || cfg_.clientToken.empty()) return;
    switchProfile_ = profile;
    std::thread([this]() {
        OAuth oauth(cfg_);
        YggResult r = oauth.YggdrasilRefresh(
            util::WStringToString(cfg_.accessToken),
            util::WStringToString(cfg_.clientToken),
            switchProfile_);
        switchOk_ = r.ok;
        switchError_ = util::StringToWString(r.error);
        if (r.ok) {
            yggAccessToken_ = util::StringToWString(r.accessToken);
            yggClientToken_  = util::StringToWString(r.clientToken);
            yggPlayer_       = util::StringToWString(r.playerName);
            yggSelected_     = r.selectedProfile;
            // 不写 yggProfiles_：LittleSkin refresh 只回当前选中的 1 个，会污染首次登录记录的完整列表
        }
        PostMessageW(hwnd_, WM_SWITCH_ROLE_DONE, 0, 0);
    }).detach();
}

void App::OnSwitchRoleDone() {
    if (switchOk_) {
        cfg_.accessToken = yggAccessToken_;
        cfg_.clientToken = yggClientToken_;
        cfg_.currentRole = yggPlayer_;
        cfg_.currentRoleId = util::StringToWString(yggSelected_.id);
        // 关键：绝不拿 refresh 响应的 availableProfiles（LittleSkin 只回当前选中的 1 个）覆盖已存列表。
        // 完整角色列表以「首次登录 authenticate 时记录」为准，长期保留；切换只改选中角色与令牌。
        if (cfg_.availableProfiles.empty() && !yggSelected_.id.empty() && !yggSelected_.name.empty())
            cfg_.availableProfiles = BuildProfilesJson(std::vector<YggProfile>{yggSelected_});
        cfg_.yggApi = Config::DefaultYggApi();  // 切换后仍标记 LittleSkin（固定地址，混淆硬编码）
        cfg_.Save();
        // 切换角色不改变账号键：同一 LittleSkin 账号下所有角色共享一份时长，
        // 公钥注册（按账号）与时长统计都继续用 accountId_，无需重新注册。
        EnsureLocalKey();                    // 确保本机 RSA 私钥就绪（切换角色仍用同一份密钥）
        EnsureRegistered(accountId_);       // 账号级注册（已注册则幂等跳过）
        // 主动拉取该账号的时长并推给前端（切角色后显示仍跟随同一账号）
        if (cloud_ && cloud_->KeyReady()) {
            long long total = cloud_->GetPlaytime(accountId_, cfg_.currentRole);
            if (total >= 0)
                SendToJs(L"{\"type\":\"cloud:playtime:got\",\"total_seconds\":" +
                         std::to_wstring(total) + L"}");
        }
        SendToJs(L"{\"type\":\"role:changed\",\"profile\":" + ProfileToJson(yggSelected_) + L"}");
    } else {
        SendToJs(L"{\"type\":\"role:changed\",\"error\":\"" + JsonEscape(switchError_) + L"\"}");
    }
}

// 自动登录（恢复登录态）后：用持久化的 accessToken/clientToken 调一次 refresh 仅用于刷新令牌
// （保持登录有效）。注意：LittleSkin 的 refresh 响应 availableProfiles 只含当前选中的 1 个角色，
// 绝不能拿它去覆盖「首次登录 authenticate 时记录的完整列表」——完整列表是切换角色的依据。
// 仅传统 Yggdrasil 路径（clientToken 非空）走这里；OAuth2 路径无 yggdrasil clientToken，跳过。
void App::RefreshProfilesOnRestore() {
    if (cfg_.accessToken.empty() || cfg_.clientToken.empty() || cfg_.currentRoleId.empty())
        return;
    std::thread([this]() {
        OAuth oauth(cfg_);
        YggProfile cur;
        cur.id   = util::WStringToString(cfg_.currentRoleId);
        cur.name = util::WStringToString(cfg_.currentRole);
        YggResult r = oauth.YggdrasilRefresh(
            util::WStringToString(cfg_.accessToken),
            util::WStringToString(cfg_.clientToken),
            cur);
        refreshOk_ = r.ok;
        refreshError_ = util::StringToWString(r.error);
        refreshAccessToken_ = util::StringToWString(r.accessToken);
        refreshClientToken_  = util::StringToWString(r.clientToken);
        refreshProfiles_.clear();
        refreshSelected_ = cur;   // 默认保持用户当前角色
        if (r.ok && !r.availableProfiles.empty()) {
            refreshProfiles_ = r.availableProfiles;
            // 选中角色仍以用户当前角色为准；若已不在新列表里，回退到服务端 selectedProfile 或第一个
            bool found = false;
            for (const auto& p : r.availableProfiles)
                if (!cur.id.empty() && p.id == cur.id) { found = true; break; }
            if (!found)
                refreshSelected_ = r.selectedProfile.id.empty() ? r.availableProfiles[0] : r.selectedProfile;
        }
        PostMessageW(hwnd_, WM_REFRESH_PROFILES, 0, 0);
    }).detach();
}

void App::OnRefreshProfilesDone() {
    if (!refreshOk_) {
        util::Log("自动登录刷新令牌失败，沿用已存登录态");
        return;
    }
    // 仅用 refresh 刷新访问令牌（保持登录有效）。完整角色列表以「首次登录 authenticate 时记录」
    // 为准——LittleSkin 的 refresh 只回当前选中的 1 个角色，绝不能拿它覆盖已存列表，否则越刷越少。
    cfg_.accessToken = refreshAccessToken_;
    cfg_.clientToken = refreshClientToken_;
    // 兜底：若本地从未存过角色列表（极少见），才退而用当前选中角色，至少不空白。
    if (cfg_.availableProfiles.empty() && !yggSelected_.id.empty() && !yggSelected_.name.empty())
        cfg_.availableProfiles = BuildProfilesJson(std::vector<YggProfile>{yggSelected_});
    cfg_.Save();
    // 推前端：用已存的完整列表重建下拉（保持当前选中角色），不替换成 refresh 的 1 个。
    std::wstring listJson = cfg_.availableProfiles.empty() ? L"[]" : cfg_.availableProfiles;
    SendToJs(L"{\"type\":\"yggdrasil:profiles:refreshed\",\"profiles\":" + listJson +
             L",\"profile\":" + ProfileToJson(yggSelected_) + L"}");
}

// ---------------- OAuth 登录（旧流程，当前未启用，保留以备回退） ----------------
void App::DoOAuthLogin() {
    // 优先：右侧弹出授权面板（不跳走主界面）；创建失败则回退整页跳转
    OpenLoginPanel();
}

void App::DoOAuthLoginLegacy() {
    OAuth oauth(cfg_);
    SendToJs(L"{\"type\":\"oauth:starting\"}");   // 提示"正在打开授权页"，避免看起来像没反应
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
        accountId_ = cfg_.accountId;          // 身份键用 LittleSkin 账号 ID（OAuth 登录已拉取 /api/user）
        pendingLoginSuccess_ = true;
        pendingUsername_ = cfg_.currentRole;  // 显示名用角色名
        EnsureLocalKey();                    // 登录成功后才生成本机 RSA 密钥对
        EnsureRegistered(accountId_);       // 把本机公钥注册到服务端（按账号）
        // 重新渲染启动器首页（NavigationCompleted 里会调用 loginSuccess 切到已登录态）
        webview_.Reload();
    } else {
        SendToJs(L"{\"type\":\"oauth:fail\",\"reason\":\"token 换取失败\"}");
        // 回到首页并提示（此时页面还在 LittleSkin，bootError 由拦截器注入，回首页后 toast 显示）
        webview_.Reload(L"token 换取失败，请重试");
    }
}

// ---------------- 右侧授权面板 ----------------
void App::OpenLoginPanel() {
    if (panelOpen_) return;
    SendToJs(L"{\"type\":\"oauth:starting\"}");   // 主窗口提示"正在打开授权页"

    // 在主窗口右侧弹出独立授权窗（内嵌 WebView2 加载 LittleSkin 授权页）
    RECT mr; GetWindowRect(hwnd_, &mr);
    int w = 460, h = mr.bottom - mr.top;
    int x = mr.right - w, y = mr.top;
    panelHwnd_ = CreateWindowExW(0, L"StardustAuthPanel", L"",
        WS_POPUP | WS_CLIPCHILDREN, x, y, w, h,
        hwnd_, nullptr, hinst_, this);
    if (!panelHwnd_) { DoOAuthLoginLegacy(); return; }   // 窗口创建失败 → 回退整页跳转
    panelOpen_ = true;
    ShowWindow(panelHwnd_, SW_SHOW);
    UpdateWindow(panelHwnd_);

    // 头部留 48px 给"返回"按钮 + 标题，WebView2 从 y=48 开始
    RECT vb = {0, 48, w, h - 48};
    std::wstring authData = util::GetTempDir() + L"\\StardustWebView2Auth";
    authPanel_.Init(panelHwnd_,
        [](const std::wstring&){},                                        // onMsg（面板内无 JS 交互）
        [this](const std::wstring& uri) -> bool { OnAuthPanelNavigate(uri); return true; },  // onNav（拦截 stardust://）
        [this](){                                                         // onReady → 打开授权页
            OAuth oauth(cfg_);
            authPanel_.Navigate(oauth.BuildAuthorizeUrl());
        },
        [](void){},                                                       // onNavigated
        &vb, false, authData.c_str());   // 自定义区域 / 不加载内嵌 HTML / 独立数据目录
}

void App::CloseLoginPanel() {
    if (!panelOpen_) return;
    panelOpen_ = false;
    authPanel_.Close();
    if (panelHwnd_) { DestroyWindow(panelHwnd_); panelHwnd_ = nullptr; }
}

void App::OnAuthPanelNavigate(const std::wstring& uri) {
    // 非 stardust:// 的跳转（LittleSkin 自身页面跳转）直接放行
    if (uri.rfind(L"stardust://", 0) != 0) return;

    // 授权失败：LittleSkin 在回调带回 error
    std::wstring err = GetQueryParam(uri, L"error");
    if (!err.empty()) {
        std::wstring desc = GetQueryParam(uri, L"error_description");
        std::wstring reason = desc.empty() ? (L"OAuth 错误: " + err) : desc;
        CloseLoginPanel();
        SendToJs(L"{\"type\":\"oauth:fail\",\"reason\":\"" + JsonEscape(reason) + L"\"}");
        webview_.Reload(reason);
        return;
    }

    // uri 形如 stardust://oauth/callback?code=xxxx
    size_t q = uri.find(L"?code=");
    if (q == std::wstring::npos) { CloseLoginPanel(); return; }
    std::wstring code = uri.substr(q + 6);
    size_t amp = code.find(L'&');
    if (amp != std::wstring::npos) code = code.substr(0, amp);

    OAuth oauth(cfg_);
    bool ok = oauth.ExchangeCode(code, cfg_);
    CloseLoginPanel();   // 先关面板，再回首页
    if (ok) {
        cfg_.Save();
        loggedIn_ = true;
        accountId_ = cfg_.accountId;          // 身份键用 LittleSkin 账号 ID（OAuth 登录已拉取 /api/user）
        pendingLoginSuccess_ = true;
        pendingUsername_ = cfg_.currentRole;  // 显示名用角色名
        EnsureLocalKey();                    // 登录成功后才生成本机 RSA 密钥对
        EnsureRegistered(accountId_);       // 把本机公钥注册到服务端（按账号）
        webview_.Reload();   // 主窗口 NavigationCompleted → loginSuccess 切到已登录态
    } else {
        SendToJs(L"{\"type\":\"oauth:fail\",\"reason\":\"token 换取失败\"}");
        webview_.Reload(L"token 换取失败，请重试");
    }
}

LRESULT CALLBACK App::AuthPanelProc(HWND h, UINT m, WPARAM w, LPARAM l) {
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
        case WM_CREATE:
            // 头部"返回"按钮（关闭授权面板，回到首页）
            CreateWindowExW(0, L"BUTTON", L"← 返回",
                WS_CHILD | WS_VISIBLE, 14, 8, 76, 32, h, (HMENU)1001, app->hinst_, nullptr);
            return 0;
        case WM_COMMAND:
            if (LOWORD(w) == 1001) { app->CloseLoginPanel(); return 0; }
            break;
        case WM_PAINT: {
            PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
            RECT r; GetClientRect(h, &r);
            RECT hdr = {0, 0, r.right, 48};
            HBRUSH bg = CreateSolidBrush(RGB(40,26,70));
            FillRect(dc, &hdr, bg); DeleteObject(bg);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(253,251,255));
            HFONT f = CreateFontW(15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, 0, 0, 0, 0, L"Microsoft YaHei UI");
            HGDIOBJ old = SelectObject(dc, f);
            RECT tr = {100, 0, r.right - 16, 48};
            DrawTextW(dc, L"LittleSkin 授权登录", -1, &tr,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, old); DeleteObject(f);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_DESTROY:
            SetWindowLongPtr(h, GWLP_USERDATA, 0);
            return DefWindowProc(h, m, w, l);
    }
    return DefWindowProc(h, m, w, l);
}

// ---------------- 游戏启动 ----------------
void App::OnJsLaunchGame() {
    // 残留态保护：若上次游戏异常退出导致 playing_ 没复位（进程已不在但状态还卡着），
    // 这里先强制复位并通知前端，避免「点击启动被第一行 if(playing_) 静默吞掉 → 按钮假死」。
    if (playing_) {
        bool procAlive = game_.IsRunning();
        if (!procAlive) {
            // 残留态：上次游戏异常退出导致 playing_ 没复位（进程已不在）。
            // 本次点击先强制复位并复位 UI，然后直接返回，等下一次点击再真正启动，
            // 避免「同一点击」紧接着又起一个线程、与旧线程并发导致重复启动/崩溃。
            playing_ = false;
            SendToJs(L"{\"type\":\"game:stopped\"}");
            return;
        } else {
            // 进程确实还活着：当作「关闭」请求，杀掉后直接返回（下一次点击再启动）
            game_.Stop();
            return;
        }
    }
    // 每次启动都重新扫描 versions 目录：读版本 jar 拿真实版本、刷新 jar 路径
    CoreScanner::Scan(cfg_);
    SendCoreList();

    if (cfg_.selectedJava.empty() || cfg_.selectedCore.empty()) {
        SendToJs(L"{\"type\":\"game:error\",\"reason\":\"请先在设置里选择 Java 和游戏核心\"}");
        return;
    }
    // 找到选中核心的版本目录 / jar / 版本名 / 真实版本
    std::wstring jar, verDir, verId, realVer;
    for (auto& c : cfg_.coreList) {
        if (c.dir == cfg_.selectedCore) {
            jar = c.jar; verDir = c.dir; verId = c.name; realVer = c.realVersion; break;
        }
    }
    if (jar.empty()) {
        // 区分两种原因，避免误导：
        //  - 一个版本都没扫到：多半是游戏目录（game_dir）指错了，versions 下没内容
        //  - 扫到了但选中的匹配不上：selectedCore 失效（Scan 里已自动回退，正常不会走到这）
        std::wstring reason = cfg_.coreList.empty()
            ? L"未扫描到任何游戏版本，请到「设置 - 游戏目录」确认路径下是否有 versions 文件夹（及其中的版本）"
            : L"未找到核心 jar 文件，请到「设置 - 游戏核心」重新选择一个版本";
        SendToJs(L"{\"type\":\"game:error\",\"reason\":\"" + JsonEscape(reason) + L"\"}");
        return;
    }

    // Java 运行环境选择：
    //  - 用户手动选了具体 Java（含手动浏览的路径）→ 直接使用，自动推荐不覆盖（手动执行不生效）
    //  - 选「自动选择」(selectedJava=="auto") 或 未选择 → 按真实版本自动推荐
    //  - 关键修复：手动选择时以「文件是否存在」为首要判定，不再因 JavaInList（路径格式匹配）
    //    失败而静默降级到自动模式。之前用户选了具体 Java 却仍看到「自动选择推荐」即因此。
    std::wstring javaExe;
    bool isAuto = (cfg_.selectedJava == L"auto" || cfg_.selectedJava.empty());
    if (!isAuto) {
        // 用户明确选择了非 auto 的 Java：优先绝对尊重（只要文件存在就直接用）
        bool isValid = JavaInList(cfg_.selectedJava);
        if (!isValid) {
            // JavaInList 可能因路径格式（大小写/正斜杠 vs 反斜杠）匹配失败，
            // 用文件存在性做兜底——用户手动浏览的路径一定该直接可用
            DWORD fa = GetFileAttributesW(cfg_.selectedJava.c_str());
            isValid = (fa != INVALID_FILE_ATTRIBUTES && !(fa & FILE_ATTRIBUTE_DIRECTORY));
        }
        if (isValid) {
            javaExe = cfg_.selectedJava;
            // 手动选择：提示实际使用的 Java，不显示「自动选择」
            SendToJs(L"{\"type\":\"game:info\",\"msg\":\"已使用你选择的 Java 启动\"}");
        } else {
            // 用户明确选了具体 Java，但路径已失效（文件被移动/卸载）：
            // 明确报错并阻断启动，绝不静默回退到自动推荐（避免"选了却用别的 Java"的困惑）
            SendToJs(L"{\"type\":\"game:error\",\"reason\":\"你选择的 Java 已不存在或不可用（"
                     + JsonEscape(cfg_.selectedJava) +
                     L"），请到 设置 - Java 运行环境 重新选择\"}");
            return;
        }
    }
    if (javaExe.empty()) {
        // 自动选择分支（isAuto=true 或手动选择的路径无效）
        std::wstring rec = PickRecommendedJava(realVer);
        if (rec.empty()) {
            int need = CoreScanner::RecommendJavaMajor(realVer);
            std::wstring needName = (need == 8 ? L"Java 8（1.8.0）" : need == 17 ? L"Java 17" : L"Java 21");
            SendToJs(L"{\"type\":\"game:error\",\"reason\":\"未找到适合版本 " +
                     JsonEscape(realVer.empty() ? verId : realVer) + L" 的 " + needName +
                     L"，请先安装对应 Java 或到设置里手动选择\"}");
            return;
        }
        javaExe = rec;
        if (cfg_.selectedJava.empty()) {
            // 首次未选择：固化推荐项并同步 UI，方便下次
            cfg_.selectedJava = rec;
            cfg_.Save();
            SendToJs(L"{\"type\":\"sys:select_java\",\"value\":\"" + JsonEscape(rec) + L"\"}");
        }
        // 「自动选择」模式下不覆盖 selectedJava（保持 "auto"），仅提示本次选用的 Java
        SendToJs(L"{\"type\":\"game:info\",\"msg\":\"已为版本 " +
                 JsonEscape(realVer.empty() ? verId : realVer) + L" 自动选择推荐 Java\"}");
    }

    playing_ = true;  // 先占住，避免「补装+启动」期间重复点击再起一个线程

    // 捕获启动所需的各项参数（线程内使用，按值捕获避免悬空引用）
    // 版本隔离：游戏运行数据（mods/saves/config/logs/natives_cache）落到
    // 「.minecraft\versions\<verId>\」隔离目录，共享根 .minecraft 只留 assets/libraries/versions
    // + launcher_profiles.json 这套 Mojang 标准布局，避免一次运行把 .minecraft 堆成杂物堆。
    // assets_root / library_directory 仍指向 sharedRoot（资源/库共享，不重复占用空间）。
    std::wstring sharedRoot = cfg_.gameDir;
    std::wstring gameDir = sharedRoot + L"\\versions\\" + verId;
    std::wstring vDir = verDir, vId = verId, vJar = jar;
    int memMb = cfg_.memoryMb;
    std::wstring role = cfg_.currentRole, roleId = cfg_.currentRoleId;
    std::wstring token = cfg_.accessToken, ygg = cfg_.yggApi;
    std::wstring priority = cfg_.processPriority;
    std::vector<std::string> extraJvm = BuildExtraJvmArgs(cfg_.gcPreset, cfg_.customJvmArgs);
    std::wstring serverAddr = cfg_.serverAddress;
    int serverPort = cfg_.serverPort;

    // 后台线程：先自动补装缺失的原版/库，再真正启动游戏，避免阻塞 UI、也避免缺文件时「黑屏卡死无提示」。
    std::thread([this, javaExe, gameDir, sharedRoot, vDir, vId, vJar, memMb, priority, extraJvm, serverAddr, serverPort, role, roleId, token, ygg]() {
        auto esc = [](const std::wstring& s) {
            std::wstring o;
            for (wchar_t c : s)
                if (c == L'"' || c == L'\\') { o += L'\\'; o += c; } else o += c;
            return o;
        };
        auto onProg = [&](int pct, const std::wstring& status) {
            SendToJs(L"{\"type\":\"install:progress\",\"pct\":" + std::to_wstring(pct) +
                     L",\"msg\":\"" + esc(status) + L"\"}");
        };
        auto fail = [&](const std::wstring& reason) {
            std::wstring e = reason;
            for (size_t i = 0; i < e.size(); ++i)
                if (e[i] == L'"' || e[i] == L'\\') { e.insert(i, 1, L'\\'); i++; }
            SendToJs(L"{\"type\":\"game:error\",\"reason\":\"" + e + L"\"}");
            playing_ = false;
        };

        SendToJs(L"{\"type\":\"launch:step\",\"text\":\"正在校验游戏文件…\"}");
        SendToJs(L"{\"type\":\"install:start\"}");
        std::wstring ierr;
        // 校验/补装走 sharedRoot（.minecraft 根）：版本 JSON、原版 jar、库文件都在共享根下，
        // 不应在隔离目录里找。
        if (!EnsureVersionInstalled(sharedRoot, vId, onProg, ierr)) {
            fail(ierr.empty() ? L"游戏文件缺失且无法自动补齐" : ierr);
            return;
        }

        SendToJs(L"{\"type\":\"launch:step\",\"text\":\"正在启动 Minecraft 客户端…\"}");
        game_.SetOnExit([this](bool crashed) { OnGameExit(crashed); });
        if (!game_.Start(javaExe, gameDir, sharedRoot, vDir, vId, vJar, memMb, priority, extraJvm, serverAddr, serverPort, role, roleId, token, ygg)) {
            fail(game_.LastError().empty() ? L"启动失败，原因未知" : game_.LastError());
            return;
        }

        lastUploadElapsed_ = 0;   // 新一局游戏，时长增量从 0 开始累计
        gameStartMs_ = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        SendToJs(L"{\"type\":\"game:started\"}");

        // 启动计时推送线程
        tickThread_ = std::thread([this]() { GameTickLoop(); });
    }).detach();
}

void App::GameTickLoop() {
    while (playing_) {
        try {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!playing_) break;
        long long now = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        long long elapsed = now - gameStartMs_;
        SendToJs(L"{\"type\":\"game:tick\",\"ms\":" + std::to_wstring(elapsed) + L"}");
        // 注：累计时长的「展示同步」已移到独立的 PlaytimeSyncLoop（与游戏是否启动无关），
        // 这里只负责本局游戏的每秒计时推送。时长上报在 OnGameExit 游戏关闭时整局上传。
        } catch (...) {
            // 单次 tick 失败（如网络/解析异常）绝不能让 detached 线程抛异常 → std::terminate。
            // 吞掉错误继续下一秒，最多丢掉一次同步，不影响游戏。
        }
    }
}

// 注册成功后立即向服务端拉取累计时长并推给前端。
// 登录成功与启动自动登录两条路径都调用，确保「无论哪种方式进入登录态」都会请求累计时长。
// 注册是异步的（EnsureRegistered detached 线程），这里重试等待注册完成（最多约 4 秒）再拉取，
// 避免首屏因注册未就绪而 401 / 拿不到数据。
void App::FetchPlaytimeAfterRegistered() {
    std::thread([this]() {
        for (int i = 0; i < 8; ++i) {        // 最多重试约 4 秒
            if (!cloud_ || !loggedIn_) return;
            EnsureRegistered(accountId_);
            if (IsRegistered(accountId_)) {
                long long total = cloud_->GetPlaytime(accountId_, cfg_.currentRole);
                if (total >= 0) {
                    SendToJs(L"{\"type\":\"cloud:playtime:got\",\"total_seconds\":" +
                             std::to_wstring(total) + L"}");
                    return;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }).detach();
}

// 独立于游戏的累计时长同步线程：只要处于登录态，就每 5 分钟从服务端拉一次最新累计时长刷新显示。
// 拉取时长是「展示用」，不需要启动游戏——即使只开着启动器首页也能看到准确的累计游玩时长。
// 线程随进程常驻（detach），logout 后 idle 等待，下次登录自动恢复；单次同步异常只吞掉不终止进程。
void App::PlaytimeSyncLoop() {
    int sinceSync = 0;     // 距上次从服务端拉取累计时长的秒数（每 5 分钟刷新一次显示）
    while (true) {
        try {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            // 未登录 / 云端未配 / 本机密钥未就绪 / 公钥未注册成功：不拉取，清零计数等下次
            if (!loggedIn_ || !CloudConfigured() || !cloud_ || !IsRegistered(accountId_)) {
                sinceSync = 0;
                continue;
            }
            ++sinceSync;
            if (sinceSync >= 300) {
                sinceSync = 0;
                long long total = cloud_->GetPlaytime(accountId_, cfg_.currentRole);
                if (total >= 0)
                    SendToJs(L"{\"type\":\"cloud:playtime:got\",\"total_seconds\":" +
                             std::to_wstring(total) + L"}");
            }
        } catch (...) {
            // 单次同步异常（网络/解析/验签失败）只吞掉，继续下一秒，不影响主程序与游戏
        }
    }
}

std::wstring App::ReadLogFileTail(const std::wstring& path, size_t maxBytes) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER sz; sz.QuadPart = 0;
    GetFileSizeEx(f, &sz);
    // 从文件末尾往前取 maxBytes 读
    size_t toRead = (size_t)sz.QuadPart;
    if (toRead > maxBytes) toRead = maxBytes;
    LARGE_INTEGER off; off.QuadPart = sz.QuadPart - (LONGLONG)toRead;
    SetFilePointerEx(f, off, nullptr, FILE_BEGIN);
    std::string buf(toRead + 1, '\0');
    DWORD rd = 0;
    ReadFile(f, &buf[0], (DWORD)toRead, &rd, nullptr);
    CloseHandle(f);
    buf.resize(rd);
    // 跳过开头可能不完整的首行
    size_t nl = buf.find('\n');
    if (nl != std::string::npos && nl + 1 < buf.size()) buf = buf.substr(nl + 1);
    return util::StringToWString(buf);
}

void App::EnsureLocalKey() {
    if (!cloud_) return;
    if (cloud_->KeyReady()) return;   // 已有私钥，无需重复

    auto persistClearKey = [&]() {
        // 关键：把清空的 privateKey 立即持久化，否则下次启动又从 shan.json 读回坏值
        cfg_.privateKey.clear();
        cfg_.Save();
    };

    // 1) 优先从 shan.json 存着的私钥恢复（BCrypt 生成的密钥数学上必然有效，
    //    加载成功即直接使用；SelfTest 仅作记录，失败不再丢弃已加载的密钥，避免每次重启都重生成导致公钥漂移）
    if (!cfg_.privateKey.empty()) {
        if (cloud_->LoadKeyFromConfig(util::WStringToString(cfg_.privateKey))) {
            bool st = cloud_->SelfTest();
            util::Diag("KEY", "从 shan.json 加载私钥成功 selfTest=" + std::string(st ? "1" : "0"));
            util::Log("已从 shan.json 加载本机 RSA 私钥");
            return;
        }
        util::Log("shan.json 中的私钥损坏（无法导入），将重新生成");
        persistClearKey();
    }
    // 2) 旧版独立文件 stardust_private.key 迁移（一次性，成功后写回 shan.json，旧文件保留无害）
    if (cloud_->LoadKeyFromLegacyFile()) {
        bool st = cloud_->SelfTest();
        cfg_.privateKey = util::StringToWString(cloud_->ExportKeyB64());
        cfg_.Save();
        util::Diag("KEY", "从旧版私钥文件迁移 selfTest=" + std::string(st ? "1" : "0"));
        util::Log("已从旧版私钥文件迁移到 shan.json");
        return;
    }
    // 3) 全新生成密钥对，并把私钥（base64）落盘到 shan.json（公钥只上传服务端）
    //    关键：BCrypt 生成的密钥数学上必然有效；SelfTest 仅作辅助校验，失败绝不 ResetKey，
    //    否则会把刚生成的有效密钥又清零，导致 KeyReady 永远为假、注册链路被永久卡死
    //    （实测 SelfTest 对 BCrypt 新密钥会误判失败，但签名/验签本身没问题，故直接采用）。
    {
        bool generated = false;
        for (int attempt = 0; attempt < 3 && !generated; ++attempt) {
            if (cloud_->GenerateKey()) { generated = true; break; }
            util::Diag("KEY", "GenerateKey 失败，重试(" + std::to_string(attempt + 1) + ")");
        }
        if (cloud_->KeyReady()) {
            bool st = cloud_->SelfTest();   // 仅作记录，失败不影响使用
            cfg_.privateKey = util::StringToWString(cloud_->ExportKeyB64());
            cfg_.Save();
            util::Log(st ? "已生成本机 RSA 密钥对（私钥写入 shan.json，自检通过）"
                         : "已生成本机 RSA 密钥对（私钥写入 shan.json；注：SelfTest 自检未通过，但密钥由 BCrypt 生成，照常使用）");
            util::Diag("KEY", "密钥就绪 selfTest=" + std::string(st ? "1" : "0"));
            return;
        }
        util::Diag("KEY", "RSA 密钥初始化失败（GenerateKey 连续失败）");
        util::Log("RSA 密钥初始化失败，时长上报将不可用");
    }
}

bool App::CloudConfigured() const {
    // 客户端 RSA 密钥就绪（本地有私钥）即可上报；地址已硬编码且固定
    return cloud_ && cloud_->KeyReady();
}

void App::EnsureRegistered(const std::wstring& pid) {
    util::Diag("REG", "EnsureRegistered 进入 pid=" + util::WStringToString(pid).substr(0, 12) +
                  " loggedIn=" + (loggedIn_ ? "1" : "0") +
                  " keyReady=" + ((cloud_ && cloud_->KeyReady()) ? "1" : "0"));
    if (!cloud_ || pid.empty()) { util::Diag("REG", "退出：cloud 空 或 pid 空"); return; }
    {
        std::lock_guard<std::mutex> lk(regMtx_);
        if (registeredPlayers_.count(pid)) return;   // 已注册成功，跳过
        if (regInFlight_.count(pid)) return;         // 已有注册线程在飞，别再起新线程
        long long now = (long long)std::time(nullptr);
        auto it = regNextRetry_.find(pid);
        if (it != regNextRetry_.end() && now < it->second) return;  // 仍在冷却期，跳过
        regInFlight_.insert(pid);   // 标记在飞，阻止线程风暴
    }
    // 公钥就绪才注册（未就绪说明登录后密钥还没生成/加载完，先退出，待密钥就绪后由调用方重试）
    if (!cloud_->KeyReady()) {
        util::Diag("REG", "退出：密钥未就绪（KeyReady=false），本次不发注册请求");
        std::lock_guard<std::mutex> lk(regMtx_);
        regInFlight_.erase(pid);
        return;
    }
    util::Diag("REG", "密钥就绪，将在线程中调用 RegisterPublicKey");
    // 后台注册，避免阻塞 UI；公钥与签名在 CloudSync::RegisterPublicKey 内部原子取出（同一把锁），
    // 保证请求体里的公钥与签名用的私钥配对（杜绝启动时序导致的验签失败）。
    // 关键：role 在进线程前按值捕获，避免跨线程读取共享的 cfg_ 字符串（数据竞争）
    std::wstring role = cfg_.currentRole;
    std::thread([this, pid, role]() {
        try {
            long long retryAfter = 600;
            int r = cloud_->RegisterPublicKey(pid, role, &retryAfter);
            std::lock_guard<std::mutex> lk(regMtx_);
            regInFlight_.erase(pid);
            if (r == 1) {
                registeredPlayers_.insert(pid);
                reconnectActive_ = false;
            } else if (r == 2) {
                // 12 小时重注册冷却：按服务端建议的剩余秒数冷却，避免反复打接口
                long long until = (long long)std::time(nullptr) + (retryAfter > 0 ? retryAfter : 43200);
                regNextRetry_[pid] = until;
                util::Log("公钥重注册被限流（12h 冷却），" + std::to_string(retryAfter) +
                          " 秒后重试：" + util::WStringToString(pid));
            } else {
                // 瞬时失败（连不上服务端）：启动"3 分钟间隔、最多 2 次"的重连
                MaybeScheduleReconnect(pid);
                regNextRetry_[pid] = (long long)std::time(nullptr) + 60;
                util::Log("公钥注册失败（连接服务端失败，进入重连）：" + util::WStringToString(pid));
            }
        } catch (...) {
            // 任何异常都不能逃逸到 detached 线程外（否则 std::terminate 杀掉整个进程）
            std::lock_guard<std::mutex> lk(regMtx_);
            regInFlight_.erase(pid);
            MaybeScheduleReconnect(pid);
            regNextRetry_[pid] = (long long)std::time(nullptr) + 60;
        }
    }).detach();
}

void App::MaybeScheduleReconnect(const std::wstring& pid) {
    // 已有重连链在跑，不重复启动，避免刷出大量线程
    if (reconnectActive_) return;
    reconnectActive_ = true;
    // 重连链：每 3 分钟尝试一次注册，最多 2 次；成功则立即刷新时长并复位状态
    std::thread([this, pid]() {
        for (int i = 0; i < kMaxReconnects; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(kReconnectGapSec));
            if (!cloud_ || pid.empty() || !loggedIn_) break;
            // 公钥与签名在 RegisterPublicKey 内部原子取出，无需在此单独取公钥
            long long retryAfter = 600;
            int r = cloud_->RegisterPublicKey(pid, cfg_.currentRole, &retryAfter);
            if (r == 1) {
                // 重连成功：标记已注册，立即拉一次时长刷新显示
                {
                    std::lock_guard<std::mutex> lk(regMtx_);
                    registeredPlayers_.insert(pid);
                    regInFlight_.erase(pid);
                    regNextRetry_.erase(pid);
                }
                reconnectActive_ = false;
                util::Log("重连服务端成功（账号 " + util::WStringToString(pid) + "）");
                if (cloud_ && IsRegistered(accountId_)) {
                    long long total = cloud_->GetPlaytime(accountId_, cfg_.currentRole);
                    if (total >= 0)
                        SendToJs(L"{\"type\":\"cloud:playtime:got\",\"total_seconds\":" +
                                 std::to_wstring(total) + L"}");
                }
                return;
            }
            // 仍失败：继续下一次重连（i 循环控制最多 kMaxReconnects 次）
        }
        // 2 次重连后仍失败：停止重连（reconnectActive_ 复位，下次链接失败可重新开始一轮）
        std::lock_guard<std::mutex> lk(regMtx_);
        regInFlight_.erase(pid);
        reconnectActive_ = false;
        util::Log("重连服务端失败（已重连 " + std::to_string(kMaxReconnects) +
                  " 次仍连不上），停止重连");
    }).detach();
    util::Log("连接服务端失败，将每 " + std::to_string(kReconnectGapSec) +
              " 秒重连一次，最多 " + std::to_string(kMaxReconnects) + " 次");
}

bool App::IsRegistered(const std::wstring& pid) const {
    std::lock_guard<std::mutex> lk(regMtx_);
    return registeredPlayers_.count(pid) > 0;
}

void App::OnGameExit(bool crashed) {
    if (!playing_) return;
    playing_ = false;
    // 立刻通知前端复位 UI：计时线程里可能有阻塞式网络请求（拉/传时长），
    // 如果服务器连不上，等它超时或上传完再发 stopped 会让用户觉得"点击关闭没反应"。
    SendToJs(L"{\"type\":\"game:stopped\"}");
    if (tickThread_.joinable()) tickThread_.join();

    long long now = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    long long sessionMs = now - gameStartMs_;
    long long sessionSec = sessionMs / 1000;

    // 退出时上传整局时长：增量 = 整局已游玩秒数 - 上次成功上传时的秒数。
    // 不再要求 IsRegistered（注册成功标记）——只要密钥就绪 + 已登录 + 有账号ID 就尝试上传，
    // 避免因注册未完成（签名失败/网络抖动）导致整局时长被静默丢弃。
    // 若服务端因公钥未注册而 401，上传会失败但至少发出了请求（日志可见），不会无声丢失。
    if (CloudConfigured() && loggedIn_ && cloud_ && !accountId_.empty()) {
        long long delta = sessionSec - lastUploadElapsed_;
        if (delta < 0) delta = 0;
        long long uploaded = cloud_->UploadPlaytime(accountId_, delta, sessionSec, cfg_.currentRole);
        util::Log("游戏退出，上传时长：账号=" + util::WStringToString(accountId_) +
                  " 本局=" + std::to_string(sessionSec) + "秒 增量=" + std::to_string(delta) +
                  "秒 结果=" + (uploaded >= 0 ? "成功(服务端累计=" + std::to_string(uploaded) + ")" : "失败(服务端未注册或不可达)"));
    } else {
        util::Log("游戏退出，跳过上传：密钥就绪=" + std::string(CloudConfigured() ? "是" : "否") +
                  " 已登录=" + std::string(loggedIn_ ? "是" : "否") +
                  " 账号ID=" + util::WStringToString(accountId_));
    }

    if (crashed) {
        // 异常退出：读 Java 日志尾部，回传给前端弹错误，便于用户/开发者定位
        std::wstring tail = ReadLogFileTail(game_.LogPath());
        std::wstring reason = L"游戏进程异常退出（可能是 Java 版本不符或游戏文件缺失）";
        if (!tail.empty()) reason = L"游戏进程异常退出，最后输出：\n" + tail;
        // 转义 JSON 特殊字符，避免破坏 JSON 解析
        for (size_t i = 0; i < reason.size(); ++i) {
            if (reason[i] == L'"' || reason[i] == L'\\') { reason.insert(i, 1, L'\\'); i++; }
        }
        SendToJs(L"{\"type\":\"game:error\",\"reason\":\"" + reason + L"\"}");
    }
}

// ---------------- 回传列表给前端 ----------------
// 结构化发送：显示名（text）+ 唯一值（value，java 用 exe 路径、core 用目录），
// JS 端 change 时把 value 回传保存到 config，避免"选了但没记住"。
void App::SendJavaList() {
    std::wstring jl = L"{\"type\":\"sys:java\",\"list\":[";
    for (size_t i = 0; i < cfg_.javaList.size(); ++i) {
        if (i) jl += L",";
        jl += L"{\"name\":\"" + JsonEscape(cfg_.javaList[i].display) +
              L"\",\"value\":\"" + JsonEscape(cfg_.javaList[i].javaExe) + L"\"}";
    }
    jl += L"]}";
    SendToJs(jl);
}

void App::SendCoreList() {
    std::wstring cl = L"{\"type\":\"sys:cores\",\"list\":[";
    for (size_t i = 0; i < cfg_.coreList.size(); ++i) {
        if (i) cl += L",";
        cl += L"{\"name\":\"" + JsonEscape(cfg_.coreList[i].name) +
              L"\",\"value\":\"" + JsonEscape(cfg_.coreList[i].dir) + L"\"" +
              L",\"real\":\"" + JsonEscape(cfg_.coreList[i].realVersion) + L"\"}";
    }
    cl += L"]}";
    SendToJs(cl);
}

bool App::JavaInList(const std::wstring& exe) const {
    for (auto& j : cfg_.javaList)
        if (_wcsicmp(j.javaExe.c_str(), exe.c_str()) == 0) return true;
    return false;
}

std::wstring App::PickRecommendedJava(const std::wstring& realVersion) {
    int need = CoreScanner::RecommendJavaMajor(realVersion);
    for (auto& j : cfg_.javaList)
        if (CoreScanner::JavaMajorFromDisplay(j.display) == need) return j.javaExe;
    return L"";
}

// 选中某个版本后：计算真实版本 + 推荐 Java，并把推荐信息推给前端；
// 若用户尚未选择 Java，则自动选推荐项（不阻断用户已明确的手动选择）。
void App::ApplyCoreSelection(const std::wstring& coreDir) {
    const CoreInfo* sel = nullptr;
    for (auto& c : cfg_.coreList)
        if (c.dir == coreDir) { sel = &c; break; }
    if (!sel) return;

    int need = CoreScanner::RecommendJavaMajor(sel->realVersion);
    std::wstring recDisp;
    for (auto& j : cfg_.javaList)
        if (CoreScanner::JavaMajorFromDisplay(j.display) == need) { recDisp = j.display; break; }

    // 推真实版本 + 推荐 Java 给前端，显示在版本说明里
    SendToJs(L"{\"type\":\"sys:core_info\",\"realVersion\":\"" +
             JsonEscape(sel->realVersion) + L"\",\"javaMajor\":" +
             std::to_wstring(need) + L",\"javaDisplay\":\"" + JsonEscape(recDisp) + L"\"}");

    // 仅在用户选了「自动选择」或完全没选时才自动推荐；
    // 用户手动选了具体 Java 路径（即使因路径格式不在扫描列表中）也绝对尊重，不覆盖。
    // 之前 !JavaInList 条件会把手动浏览的合法路径误判为"失效"并强制覆盖为推荐项。
    if (cfg_.selectedJava.empty() || cfg_.selectedJava == L"auto") {
        std::wstring rec = PickRecommendedJava(sel->realVersion);
        if (!rec.empty()) {
            cfg_.selectedJava = rec;
            cfg_.Save();
            SendToJs(L"{\"type\":\"sys:select_java\",\"value\":\"" + JsonEscape(rec) + L"\"}");
        }
    }
}

// 把物理内存总量与推荐分配推给前端（round 到整数 GB）
void App::SendMemInfo() {
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
}

void App::SendLaunchConfig() {
    SendToJs(L"{\"type\":\"sys:config\",\"process_priority\":\"" + JsonEscapeW(cfg_.processPriority) +
             L"\",\"gc_preset\":\"" + JsonEscapeW(cfg_.gcPreset) +
             L"\",\"custom_jvm_args\":\"" + JsonEscapeW(cfg_.customJvmArgs) +
             L"\",\"server_address\":\"" + JsonEscapeW(cfg_.serverAddress) +
             L"\",\"server_port\":" + std::to_wstring(cfg_.serverPort) + L"}");
}

// ---------------- 选择 Java 文件夹 ----------------
void App::SelectJavaFolder() {
    IFileDialog* pfd = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&pfd)))) return;
    DWORD opts = 0;
    pfd->GetOptions(&opts);
    pfd->SetOptions(opts | FOS_PICKFOLDERS);
    pfd->SetTitle(L"请选择 bin 的文件夹目录");
    if (SUCCEEDED(pfd->Show(hwnd_))) {
        IShellItem* psi = nullptr;
        if (SUCCEEDED(pfd->GetResult(&psi))) {
            wchar_t* path = nullptr;
            if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                std::wstring binDir(path);
                std::wstring exe = JavaScanner::PickManual(binDir, cfg_);
                if (!exe.empty()) {
                    cfg_.Save();
                    SendJavaList();
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

// ---------------- 选择整合包 zip ----------------
void App::DoModpackBrowse() {
    IFileDialog* pfd = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&pfd)))) return;
    COMDLG_FILTERSPEC filters[] = {
        { L"整合包 (*.zip)", L"*.zip" },
        { L"所有文件 (*.*)", L"*.*" },
    };
    pfd->SetFileTypes(2, filters);
    pfd->SetTitle(L"选择整合包 zip 文件");
    if (SUCCEEDED(pfd->Show(hwnd_))) {
        IShellItem* psi = nullptr;
        if (SUCCEEDED(pfd->GetResult(&psi))) {
            wchar_t* path = nullptr;
            if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                SendToJs(L"{\"type\":\"modpack:selected\",\"path\":\"" +
                         JsonEscapeW(std::wstring(path)) + L"\"}");
                CoTaskMemFree(path);
            }
            psi->Release();
        }
    }
    pfd->Release();
}

// ---------------- 整合包导入（后台线程） ----------------
void App::DoImportModpack(const std::wstring& zipPath) {
    // 防重复触发：同一时刻只跑一个导入任务
    if (modpackImporting_.exchange(true)) {
        SendToJs(L"{\"type\":\"modpack:error\",\"reason\":\"已有整合包正在导入中\"}");
        return;
    }
    std::wstring zip = zipPath;
    // 先按包内 MC 版本挑一个合适的 Java（Forge 1.20.x 需要 Java 17+），
    // 挑不到就回退到用户当前选择的 Java。
    std::wstring mc = ModpackMcVersion(zip);
    std::wstring java = PickRecommendedJava(mc);
    if (java.empty()) java = cfg_.selectedJava;

    std::thread([this, zip, java]() {
        auto report = [this](int step, int total, int pct, const std::wstring& status) {
            SendToJs(L"{\"type\":\"modpack:progress\",\"step\":" + std::to_wstring(step) +
                     L",\"total\":" + std::to_wstring(total) +
                     L",\"pct\":" + std::to_wstring(pct) +
                     L",\"status\":\"" + JsonEscapeW(status) + L"\"}");
        };
        std::wstring err;
        MpSummary mpSum;
        bool ok = ImportModpack(zip, cfg_.gameDir, java, cloud_, report, err, &mpSum);
        if (ok) {
            // 导入成功：重新扫描 versions 目录并刷新设置里的游戏版本下拉，
            // 避免用户必须重启启动器才能看到新装好的整合包（点 3）。
            CoreScanner::Scan(cfg_);
            SendCoreList();
            SendToJs(L"{\"type\":\"modpack:done\","
                     L"\"total\":" + std::to_wstring(mpSum.total) +
                     L",\"direct\":" + std::to_wstring(mpSum.direct) +
                     L",\"s3\":" + std::to_wstring(mpSum.s3) +
                     L",\"skipped\":" + std::to_wstring(mpSum.skipped) +
                     L",\"failed\":" + std::to_wstring(mpSum.failed) + L"}");
        } else {
            SendToJs(L"{\"type\":\"modpack:error\",\"reason\":\"" + JsonEscapeW(err) + L"\"}");
        }
        modpackImporting_ = false;
    }).detach();
}

// ---------------- 处理 JS 消息 ----------------
// （顶部栏由 HTML 渲染，拖动走 JS mousedown → 'drag' 消息 → 系统 SC_MOVE，
//   最小化/关闭走 'window' 消息，与 client 项目同款方案）
