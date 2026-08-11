#pragma once
// 内嵌资源 ID（全部编进 exe，程序目录不再需要任何外部文件）

// 窗口 / 任务栏 / EXE 文件图标（ICO 多尺寸），来自 QQ图片20260811091948.jpg 裁剪
#define IDI_APP_ICON       100

// 内嵌的 launcher.html（RT_HTML 预定义类型），运行时由 webview2_host 读取并通过
// 虚拟域名 https://app.stardust.local/* 的请求拦截返回给页面 —— 背景图不做 base64
// 注入 HTML，而是用 CSS 变量占位，运行时由 webview2_host 读取 IDR_BG_PNG →
// base64 → setProperty('--bg') 注入。
// 注：WebView2 loader 采用静态链接（WebView2LoaderStatic.lib），无 WebView2Loader.dll。
#define IDR_APP_HTML      200
#define IDR_BG_PNG        201   // 背景图 PNG（RCDATA），运行时 base64 注入
#define IDR_LOGO_PNG      202   // 左上角头像 logo（RCDATA），运行时 base64 注入
