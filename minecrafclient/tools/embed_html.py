#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
构建工具：把 res/background.png 转 base64 data URI 注入 launcher.html，
生成「内嵌版」HTML，供 RC 编译器以 RT_HTML 资源编译进 exe。

原因：WebView2 的 NavigateToString 无法引用外部文件（file:// 相对路径失效），
所以背景图必须在构建时内联进 HTML 本体。

用法（由 CMake 在配置阶段调用）：
    python embed_html.py --html <源 launcher.html> --png <背景图> --out <输出 html>
"""
import argparse
import base64
import pathlib
import re
import sys


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--html", required=True, help="源 launcher.html 绝对路径")
    ap.add_argument("--png", required=True, help="背景图 background.png 绝对路径")
    ap.add_argument("--out", required=True, help="输出内嵌版 HTML 绝对路径")
    args = ap.parse_args()

    html_path = pathlib.Path(args.html)
    png_path = pathlib.Path(args.png)
    out_path = pathlib.Path(args.out)

    if not html_path.exists():
        print(f"[embed_html] 错误: HTML 不存在: {html_path}", file=sys.stderr)
        return 1
    if not png_path.exists():
        print(f"[embed_html] 错误: 背景图不存在: {png_path}", file=sys.stderr)
        return 1

    html = html_path.read_text(encoding="utf-8")
    png = png_path.read_bytes()
    b64 = base64.b64encode(png).decode("ascii")
    data_uri = "data:image/png;base64," + b64

    # 兼容 url("background.png") / url(background.png) / url('background.png')
    pattern = re.compile(r'url\(\s*["\']?background\.png["\']?\s*\)')
    if not pattern.search(html):
        print(f"[embed_html] 警告: HTML 中未找到 background.png 引用，跳过注入", file=sys.stderr)
    html = pattern.sub('url("' + data_uri + '")', html)

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(html, encoding="utf-8")
    print(f"[embed_html] 注入完成: 背景图 {len(png) // 1024} KB → base64 {len(b64) // 1024} KB，输出 {out_path}")
    return 0


if __name__ == "__main__":
    # CI 的 Windows 控制台默认代码页可能是 cp1252，直接 print 中文会抛
    # UnicodeEncodeError 导致构建失败。强制 stdout/stderr 走 UTF-8，
    # 编码失败用 ? 替换，保证脚本永不因输出问题崩掉（CMake 只看退出码）。
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    sys.exit(main())
