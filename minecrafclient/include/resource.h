#pragma once
// 内嵌资源 ID（HTML 编译进 exe，程序目录不再需要 res\ 文件夹）

// 内嵌的 launcher.html（RT_HTML 预定义类型），运行时由 webview2_host 读取并
// NavigateToString 渲染 —— NavigateToString 无法引用外部文件，因此背景图
// 已由 tools/embed_html.py 在构建时转成 base64 data URI 注入 HTML。
#define IDR_APP_HTML 200
