#pragma once
// Java 扫描：多种方式找本机 Java，支持手动选择 bin 目录
#include <string>
#include <vector>
#include "config.h"

class JavaScanner {
public:
    // 扫描所有 Java 并填充到 Config
    static void Scan(Config& cfg);

    // 手动选择 Java 的 bin 文件夹，验证后加入列表
    // 返回空串表示失败，否则返回 java.exe 完整路径
    static std::wstring PickManual(const std::wstring& binDir, Config& cfg);

private:
    static void AddIfValid(std::vector<JavaInfo>& out, const std::wstring& javaExe);
    static std::wstring QueryVersion(const std::wstring& javaExe); // 运行 java -version 取版本
    static void ScanEnvJava(std::vector<JavaInfo>& out);
    static void ScanRegistry(std::vector<JavaInfo>& out);
    static void ScanCommonDirs(std::vector<JavaInfo>& out);
    static void ScanPath(std::vector<JavaInfo>& out);
};
