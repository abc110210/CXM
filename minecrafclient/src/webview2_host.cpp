// WebView2 封装实现
#include "webview2_host.h"
#include "resource.h"
#include "common.h"
#include <wrl.h>
#include <shlwapi.h>
#include <string>
#include <vector>
#include <delayimp.h>   // delayload 钩子（运行期 dll 释放兜底）

using namespace Microsoft::WRL;

WebView2Host::WebView2Host() {}
WebView2Host::~WebView2Host() {}

// ========== 资源读取 ==========

// 从 exe 资源读取内嵌 HTML（RT_HTML -> IDR_APP_HTML）
std::wstring WebView2Host::LoadAppHtml() {
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    HRSRC hRes = FindResourceW(hInst, MAKEINTRESOURCEW(IDR_APP_HTML), RT_HTML);
    if (!hRes) {
        util::Log("LoadAppHtml: FindResourceW(IDR_APP_HTML, RT_HTML) 失败，HTML 未嵌入 exe");
        return L"";
    }
    HGLOBAL hGlob = LoadResource(hInst, hRes);
    if (!hGlob) {
        util::Log("LoadAppHtml: LoadResource 失败");
        return L"";
    }
    DWORD size = SizeofResource(hInst, hRes);
    const char* data = (const char*)LockResource(hGlob);
    if (!data || size == 0) {
        util::Log("LoadAppHtml: 资源为空");
        return L"";
    }
    std::string utf8(data, (size_t)size);
    return util::StringToWString(utf8);
}

// 从 exe 资源读取背景图 PNG（RCDATA -> IDR_BG_PNG）
std::vector<uint8_t> WebView2Host::LoadBackgroundPng() {
    std::vector<uint8_t> png;
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    HRSRC hRes = FindResourceW(hInst, MAKEINTRESOURCEW(IDR_BG_PNG), RT_RCDATA);
    if (!hRes) {
        util::Log("LoadBackgroundPng: FindResourceW(IDR_BG_PNG, RT_RCDATA) 失败");
        return png;
    }
    HGLOBAL hGlob = LoadResource(hInst, hRes);
    if (!hGlob) return png;
    DWORD size = SizeofResource(hInst, hRes);
    const uint8_t* data = (const uint8_t*)LockResource(hGlob);
    if (!data || size == 0) return png;
    png.assign(data, data + size);
    return png;
}

// ========== 运行时释放 WebView2Loader.dll ==========
// 程序目录如果已经有 dll（开发期），直接复用；否则从 exe 资源释放到 %TEMP%。
// 这样程序目录最终只剩 exe + config.ini，dll 走系统临时区（与 WebView2 缓存同盘）。
std::wstring WebView2Host::EnsureWebView2LoaderDll() {
    // 1) 优先复用程序目录的 dll（开发/调试期更顺手）
    std::wstring exeDir = util::GetExeDir();
    std::wstring exeSide = exeDir + L"\\WebView2Loader.dll";
    if (GetFileAttributesW(exeSide.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return exeSide;
    }
    // 2) 否则从资源释放到 %TEMP%
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    HRSRC hRes = FindResourceW(hInst, MAKEINTRESOURCEW(IDR_WEBVIEW2_DLL), RT_RCDATA);
    if (!hRes) {
        util::Log("EnsureWebView2LoaderDll: 资源 IDR_WEBVIEW2_DLL 缺失");
        return L"";
    }
    HGLOBAL hGlob = LoadResource(hInst, hRes);
    if (!hGlob) return L"";
    DWORD size = SizeofResource(hInst, hRes);
    const uint8_t* data = (const uint8_t*)LockResource(hGlob);
    if (!data || size == 0) return L"";

    std::wstring outPath = util::GetTempDir() + L"\\StardustWebView2Loader.dll";
    HANDLE h = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        util::Log("EnsureWebView2LoaderDll: CreateFileW 失败: " + std::to_string(GetLastError()));
        return L"";
    }
    DWORD written = 0;
    BOOL ok = WriteFile(h, data, size, &written, nullptr);
    CloseHandle(h);
    if (!ok || written != size) {
        util::Log("EnsureWebView2LoaderDll: WriteFile 失败: " + std::to_string(GetLastError()));
        return L"";
    }
    // LoadLibrary（不保存句柄，进程退出时系统自动清理；启动器短生命周期足够）
    if (!LoadLibraryW(outPath.c_str())) {
        util::Log("EnsureWebView2LoaderDll: LoadLibraryW 失败: " + std::to_string(GetLastError()));
        return L"";
    }
    return outPath;
}

// ========== 注入背景图 ==========
// 把 PNG → base64 → setProperty('--bg', 'url(data:image/png;base64,...)')。
// 在每次 NavigationCompleted 后调用一次（首次加载 + OAuth Reload）。
void WebView2Host::InjectBackground() {
    if (!webview_) return;
    static std::vector<uint8_t> cached;  // 多次导航时复用，避免重复读资源
    if (cached.empty()) cached = LoadBackgroundPng();
    if (cached.empty()) return;

    // base64 编码
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string b64;
    b64.reserve(((cached.size() + 2) / 3) * 4);
    for (size_t i = 0; i < cached.size(); i += 3) {
        uint32_t n = (uint32_t)cached[i] << 16;
        if (i + 1 < cached.size()) n |= (uint32_t)cached[i + 1] << 8;
        if (i + 2 < cached.size()) n |= (uint32_t)cached[i + 2];
        b64.push_back(tbl[(n >> 18) & 0x3F]);
        b64.push_back(tbl[(n >> 12) & 0x3F]);
        b64.push_back(i + 1 < cached.size() ? tbl[(n >> 6) & 0x3F] : '=');
        b64.push_back(i + 2 < cached.size() ? tbl[n & 0x3F] : '=');
    }

    std::wstring js = L"document.documentElement.style.setProperty('--bg','url(\"data:image/png;base64," +
                      util::StringToWString(b64) + L"\")')";
    webview_->ExecuteScript(js.c_str(), nullptr);
}

// ========== Init ==========

HRESULT WebView2Host::Init(HWND hwnd,
                           MessageHandler onMsg, NavHandler onNav,
                           ReadyHandler onReady, ReadyHandler onNavigated) {
    hwnd_ = hwnd;
    onMsg_ = onMsg;
    onNav_ = onNav;
    onReady_ = onReady;
    onNavigated_ = onNavigated;

    // 先确保 WebView2Loader.dll 可被加载（不在程序目录时从 exe 资源释放到 %TEMP%）
    if (EnsureWebView2LoaderDll().empty()) {
        util::Log("Init: WebView2Loader.dll 加载失败，无法启动 WebView2");
        return E_FAIL;
    }

    // 从 exe 资源读取内嵌 HTML；为空说明资源没编进去，直接报错
    html_ = LoadAppHtml();
    if (html_.empty()) {
        util::Log("Init: 未找到内嵌 HTML 资源，无法启动界面");
        return E_FAIL;
    }

    // WebView2 缓存、Cookie 等统一放系统临时目录 %TEMP%\StardustWebView2，
    // 程序目录保持干净（与 client 项目同策略）
    std::wstring userData = util::GetWebView2DataDir();

    return CreateCoreWebView2EnvironmentWithOptions(
        nullptr, userData.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this](HRESULT, ICoreWebView2Environment* env) -> HRESULT {
                if (!env) return E_FAIL;
                env->CreateCoreWebView2Controller(hwnd_,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [this](HRESULT, ICoreWebView2Controller* ctrl) -> HRESULT {
                            if (!ctrl) return E_FAIL;
                            controller_ = ctrl;
                            ctrl->get_CoreWebView2(&webview_);

                            // 精装原生感：禁用右键菜单、DevTools、状态栏、缩放、浏览器快捷键
                            ComPtr<ICoreWebView2Settings> settings;
                            if (SUCCEEDED(webview_->get_Settings(&settings))) {
                                settings->put_AreDefaultContextMenusEnabled(FALSE);
                                settings->put_AreDevToolsEnabled(FALSE);
                                settings->put_IsStatusBarEnabled(FALSE);
                                settings->put_IsZoomControlEnabled(FALSE);
                                ComPtr<ICoreWebView2Settings3> settings3;
                                if (SUCCEEDED(settings.As(&settings3))) {
                                    settings3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
                                }
                            }

                            RECT r; GetClientRect(hwnd_, &r);
                            ctrl->put_Bounds(r);

                            // 注册 WebMessage 接收
                            webview_->add_WebMessageReceived(
                                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [this](IUnknown*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                        LPWSTR json = nullptr;
                                        args->get_WebMessageAsJson(&json);
                                        if (json) {
                                            onMsg_(std::wstring(json));
                                            CoTaskMemFree(json);
                                        }
                                        return S_OK;
                                    }).Get(), nullptr);

                            // 注册导航开始：拦截 stardust:// 协议回调
                            webview_->add_NavigationStarting(
                                Callback<ICoreWebView2NavigationStartingEventHandler>(
                                    [this](IUnknown*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                                        LPWSTR uri = nullptr;
                                        args->get_Uri(&uri);
                                        if (uri) {
                                            std::wstring u(uri);
                                            CoTaskMemFree(uri);
                                            if (u.rfind(L"stardust://", 0) == 0) {
                                                onNav_(u);
                                                args->put_Cancel(TRUE);
                                                return S_OK;
                                            }
                                        }
                                        return S_OK;
                                    }).Get(), nullptr);

                            // 注册导航完成：用于登录后回到首页时切到已登录态 + 注入背景图
                            webview_->add_NavigationCompleted(
                                Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                    [this](IUnknown*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                                        if (onNavigated_) onNavigated_();
                                        InjectBackground();   // 幂等：每次导航后都注入一次
                                        return S_OK;
                                    }).Get(), nullptr);

                            // 导航到内嵌 HTML（NavigateToString 直接在内存渲染，不依赖外部文件）
                            webview_->NavigateToString(html_.c_str());

                            ready_ = true;
                            if (onReady_) onReady_();
                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());
}

void WebView2Host::PostMessage(const std::wstring& json) {
    if (webview_) webview_->PostWebMessageAsJson(json.c_str());
}

void WebView2Host::ExecuteScript(const std::wstring& js) {
    if (!webview_) return;
    webview_->ExecuteScript(js.c_str(), nullptr);
}

void WebView2Host::Navigate(const std::wstring& urlOrFile) {
    if (!webview_) return;
    std::wstring lower = urlOrFile;
    for (auto& c : lower) c = towlower(c);
    if (lower.rfind(L"http://", 0) == 0 || lower.rfind(L"https://", 0) == 0 ||
        lower.rfind(L"stardust://", 0) == 0) {
        webview_->Navigate(urlOrFile.c_str());
    } else {
        wchar_t url[MAX_PATH] = {0};
        DWORD urlLen = MAX_PATH;
        if (SUCCEEDED(UrlCreateFromPathW(urlOrFile.c_str(), url, &urlLen, 0)))
            webview_->Navigate(url);
        else
            webview_->Navigate(urlOrFile.c_str());
    }
}

// OAuth 登录回调后回到启动器首页：重新用内嵌 HTML 渲染
void WebView2Host::Reload() {
    if (webview_ && !html_.empty())
        webview_->NavigateToString(html_.c_str());
}

void WebView2Host::Resize() {
    if (controller_) {
        RECT r; GetClientRect(hwnd_, &r);
        controller_->put_Bounds(r);
    }
}

// ========== 延迟加载失败钩子（兜底释放 dll） ==========
// MSVC /DELAYLOAD:WebView2Loader.dll 让 WebView2Loader.dll 在第一次调用
// WebView2 API 时才 LoadLibrary。若此时 EnsureWebView2LoaderDll 还没跑过
// （极端时序），loader 会失败；钩子在此场景下从 RCDATA 释放 dll 到 %TEMP%
// 并返回 HMODULE，让延迟加载器继续解析函数。
extern "C" {
FARPROC WINAPI WebView2DelayLoadFailureHook(unsigned int dliNotify, PDelayLoadInfo pdliInfo) {
    if (dliNotify == dliFailLoadLib && pdliInfo && pdliInfo->szDll
        && _stricmp(pdliInfo->szDll, "WebView2Loader.dll") == 0) {
        std::wstring path = WebView2Host::EnsureWebView2LoaderDll();
        if (!path.empty()) {
            HMODULE h = LoadLibraryW(path.c_str());
            if (h) return reinterpret_cast<FARPROC>(h);  // 把我们的 HMODULE 给延迟加载器
        }
    }
    return NULL;  // 让默认错误处理弹窗（其他 dll 失败时仍正常报错）
}

// 注册到 MSVC 延迟加载器（链接器会调用这个符号）
__declspec(selectany) PfnDliHook __pfnDliFailureHook2 = WebView2DelayLoadFailureHook;
} // extern "C"