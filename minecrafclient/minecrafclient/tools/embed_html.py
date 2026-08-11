#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
构建工具：把 res/launcher.html 里 background.png 的引用替换成短占位符。

重要：这里只做"占位符替换"，不做 base64 注入。
原因：WebView2 的 NavigateToString 对超大 HTML 会静默失败（背景图 1.1MB
转 base64 后 1.55MB，HTML 注入后超 1.6MB 经常白屏）。所以背景图改为运行时
RCDATA 注入：background.png 作为独立资源编进 exe，运行时由 webview2_host
读出来 base64 编码后通过 CSS 变量 --bg 注入到页面（保持 HTML 本体 < 50KB）。

HTML 源里 url("background.png") 占位为 url("BG_PLACEHOLDER")，构建产物里
就是 url("BG_PLACEHOLDER")，运行时由 C++ 替换为 url("data:image/png;base64,...")。
"""
import argparse
import pathlib
import sys

# 占位符：必须是一个不会被任何合法 CSS 误判的字符串，且尽量短
PLACEHOLDER = "BG_PLACEHOLDER"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--html", required=True, help="源 launcher.html 绝对路径")
    ap.add_argument("--out", required=True, help="输出内嵌版 HTML 绝对路径")
    args = ap.parse_args()

    html_path = pathlib.Path(args.html)
    out_path = pathlib.Path(args.out)

    if not html_path.exists():
        print(f"[embed_html] 错误: HTML 不存在: {html_path}", file=sys.stderr)
        return 1

    html = html_path.read_text(encoding="utf-8")
    # 兼容 url("background.png") / url(background.png) / url('background.png')
    # HTML 源已经用占位符 BG_PLACEHOLDER 时也兼容（幂等替换）
    import re
    replaced = re.sub(
        r'url\(\s*["\']?(?:background\.png|BG_PLACEHOLDER)["\']?\s*\)',
        f'url("{PLACEHOLDER}")',
        html,
    )
    if replaced == html and PLACEHOLDER not in html:
        print(f"[embed_html] 警告: HTML 中未找到 background.png / BG_PLACEHOLDER 引用，跳过替换", file=sys.stderr)
    html = replaced

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(html, encoding="utf-8")
    print(f"[embed_html] 占位符替换完成 → {out_path}")
    return 0


if __name__ == "__main__":
    # CI 的 Windows 控制台默认代码页可能是 cp1252，构建脚本的 print 输出
    # 中文可能 UnicodeEncodeError 导致整个构建失败。强制 UTF-8 + 失败替换。
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    sys.exit(main())