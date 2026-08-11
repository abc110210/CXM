#pragma once
// WebView2 封装：加载 HTML、收发 JS 消息、拦截自定义协议导航、执行脚本
#include <windows.h>
#include <wrl.h>
#include <WebView2.h>
#include <string>
#include <functional>

class WebView2Host {
public:
    // JS 发来的消息（已转成 wstring JSON）
    using MessageHandler = std::function<void(const std::wstring&)>;
    // 导航开始：返回 true 表示取消该导航（用于拦截 stardust:// 回调）
    using NavHandler = std::function<bool(const std::wstring& uri)>;
    // WebView 初始化完成 / 每次导航完成
    using ReadyHandler = std::function<void()>;

    WebView2Host();
    ~WebView2Host();

    // 初始化 WebView2，hwnd 为承载窗口。
    // 网页（launcher.html）已内嵌进 exe 资源（RT_HTML），此处自动读取并 NavigateToString。
    HRESULT Init(HWND hwnd,
                 MessageHandler onMsg, NavHandler onNav,
                 ReadyHandler onReady, ReadyHandler onNavigated);

    // 向 JS 发送消息（JSON 字符串）
    void PostMessage(const std::wstring& json);

    // 在页面里执行 JS 脚本（用于登录后切到已登录态等）
    void ExecuteScript(const std::wstring& js);

    // 导航到指定 URL 或本地文件（自动转 file://）；用于登录页等外部导航
    void Navigate(const std::wstring& urlOrFile);

    // 重新加载内嵌 HTML（OAuth 登录回调后回到启动器首页用）
    void Reload();

    // 窗口尺寸变化时调用
    void Resize();

private:
    // 从当前 exe 资源读取内嵌 HTML（RT_HTML -> IDR_APP_HTML），失败返回空串
    static std::wstring LoadAppHtml();

    Microsoft::WRL::ComPtr<ICoreWebView2> webview_;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller_;
    HWND hwnd_ = nullptr;
    std::wstring html_;          // 内嵌 HTML 内容（NavigateToString 渲染）
    MessageHandler onMsg_;
    NavHandler onNav_;
    ReadyHandler onReady_;
    ReadyHandler onNavigated_;
    bool ready_ = false;
};
