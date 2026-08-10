#pragma once
// 通用工具：字符串编码转换、日志输出、路径处理
#include <windows.h>
#include <string>
#include <vector>
#include <iostream>

namespace util {

// UTF-16 (wstring) <-> UTF-8 (string) 转换
inline std::string WStringToString(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], len, nullptr, nullptr);
    return out;
}

inline std::wstring StringToWString(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], len);
    return out;
}

// 简单日志（输出到控制台，Release 也可见）
inline void Log(const std::string& msg) {
    std::cout << "[Stardust] " << msg << std::endl;
}

// 字节转十六进制字符串（用于 HMAC 结果展示）
inline std::string BytesToHex(const std::vector<uint8_t>& bytes) {
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) {
        out.push_back(hex[b >> 4]);
        out.push_back(hex[b & 0x0F]);
    }
    return out;
}

// 取当前 exe 所在目录
inline std::wstring GetExeDir() {
    wchar_t buf[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring path(buf);
    size_t pos = path.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? L"." : path.substr(0, pos);
}

} // namespace util
