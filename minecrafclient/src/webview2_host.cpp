// WebView2 封装实现
#include "webview2_host.h"
#include "common.h"
#include <wrl.h>
#include <shlwapi.h>
#include <string>

using namespace Microsoft::WRL;

WebView2Host::WebView2Host() {}
WebView2Host::~WebView2Host() {}

HRESULT WebView2Host::Init(HWND hwnd, const std::wstring& htmlFile,
                           MessageHandler onMsg, NavHandler onNav,
                           ReadyHandler onReady, ReadyHandler onNavigated) {
    hwnd_ = hwnd;
    htmlFile_ = htmlFile;
    onMsg_ = onMsg;
    onNav_ = onNav;
    onReady_ = onReady;
    onNavigated_ = onNavigated;

    // WebView2 缓存、Cookie 等统一放系统临时目录 %TEMP%\StardustWebView2，
    // 程序目录保持干净，只保留 exe / dll / res / config.ini（与 client 项目同策略）
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
                                // 浏览器快捷键在 ICoreWebView2Settings3 上，按需查询后设置
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
                                                onNav_(u);              // 交给主程序解析 code
                                                args->put_Cancel(TRUE); // 取消跳转，不真正离开
                                                return S_OK;
                                            }
                                        }
                                        return S_OK;
                                    }).Get(), nullptr);

                            // 注册导航完成：用于登录后回到首页时切到已登录态
                            webview_->add_NavigationCompleted(
                                Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                    [this](IUnknown*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                                        if (onNavigated_) onNavigated_();
                                        return S_OK;
                                    }).Get(), nullptr);

                            // 导航到本地 html（自动转 file:// URL 并编码）
                            wchar_t url[MAX_PATH] = {0};
                            DWORD urlLen = MAX_PATH;
                            if (SUCCEEDED(UrlCreateFromPathW(htmlFile_.c_str(), url, &urlLen, 0)))
                                webview_->Navigate(url);
                            else
                                webview_->Navigate(htmlFile_.c_str());

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
        // 本地文件转 file:// URL
        wchar_t url[MAX_PATH] = {0};
        DWORD urlLen = MAX_PATH;
        if (SUCCEEDED(UrlCreateFromPathW(urlOrFile.c_str(), url, &urlLen, 0)))
            webview_->Navigate(url);
        else
            webview_->Navigate(urlOrFile.c_str());
    }
}

void WebView2Host::Resize() {
    if (controller_) {
        RECT r; GetClientRect(hwnd_, &r);
        controller_->put_Bounds(r);
    }
}
