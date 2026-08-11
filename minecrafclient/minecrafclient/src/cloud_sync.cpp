// 云端同步实现
#include "cloud_sync.h"
#include "common.h"
#include <bcrypt.h>
#include <winhttp.h>
#include <ctime>
#include <vector>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "winhttp.lib")

CloudSync::CloudSync(const std::wstring& serverUrl, const std::wstring& secret)
    : serverUrl_(serverUrl), secret_(secret) {}

// ---------------- HMAC-SHA256 ----------------
std::string CloudSync::MakeToken(const std::wstring& playerId, long long timestamp) {
    std::string msg = "player_id=" + util::WStringToString(playerId) + "&ts=" + std::to_string(timestamp);
    std::string key = util::WStringToString(secret_);

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) != 0)
        return {};

    BCRYPT_HASH_HANDLE hHash = nullptr;
    if (BCryptCreateHash(hAlg, &hHash, nullptr, 0, (PUCHAR)key.data(), (ULONG)key.size(), 0) != 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return {};
    }
    BCryptHashData(hHash, (PUCHAR)msg.data(), (ULONG)msg.size(), 0);

    std::vector<uint8_t> out(32);
    BCryptFinishHash(hHash, out.data(), (ULONG)out.size(), 0);
    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);

    return util::BytesToHex(out);
}

// ---------------- WinHTTP 请求 ----------------
std::string CloudSync::HttpJson(const std::wstring& method,
                                 const std::wstring& path,
                                 const std::wstring& playerId,
                                 const std::string& bodyJson) {
    long long ts = (long long)time(nullptr);
    std::string token = MakeToken(playerId, ts);

    // 解析 serverUrl_ -> 主机名 + 端口
    // 期望形如 https://playtime.yourdomain.com
    std::wstring host, port = L"443";
    size_t pos = serverUrl_.find(L"://");
    std::wstring rest = (pos == std::wstring::npos) ? serverUrl_ : serverUrl_.substr(pos + 3);
    size_t slash = rest.find(L'/');
    std::wstring authority = (slash == std::wstring::npos) ? rest : rest.substr(0, slash);
    size_t colon = authority.find(L':');
    if (colon != std::wstring::npos) {
        host = authority.substr(0, colon);
        port = authority.substr(colon + 1);
    } else {
        host = authority;
    }

    std::wstring fullPath = path.empty() ? L"/" : path;
    if (fullPath[0] != L'/') fullPath = L"/" + fullPath;

    HINTERNET hSession = WinHttpOpen(L"StardustLauncher/1.0",
                                      WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return {};

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(),
                                        (INTERNET_PORT)_wtoi(port.c_str()), 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return {}; }

    DWORD flags = WINHTTP_FLAG_SECURE; // HTTPS
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, method.c_str(), fullPath.c_str(),
                                            nullptr, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return {}; }

    // 允许自签/部分证书错误（生产可去掉）
    DWORD secure = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_CN_INVALID
                 | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &secure, sizeof(secure));

    std::wstring headers = L"Content-Type: application/json\r\n"
                           L"X-Player-ID: " + playerId + L"\r\n"
                           L"X-Timestamp: " + std::to_wstring(ts) + L"\r\n"
                           L"X-Token: " + util::StringToWString(token) + L"\r\n";

    if (!WinHttpSendRequest(hRequest, headers.c_str(), (DWORD)-1L,
                            (LPVOID)bodyJson.data(), (DWORD)bodyJson.size(),
                            (DWORD)bodyJson.size(), 0)) {
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        return {};
    }
    if (!WinHttpReceiveResponse(hRequest, nullptr)) {
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        return {};
    }

    // 读取响应
    std::string response;
    DWORD available = 0, read = 0;
    do {
        available = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &available)) break;
        if (available == 0) break;
        std::vector<char> buf(available + 1, 0);
        if (!WinHttpReadData(hRequest, buf.data(), available, &read)) break;
        response.append(buf.data(), read);
    } while (available > 0);

    WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
    return response;
}

long long CloudSync::GetPlaytime(const std::wstring& playerId) {
    // 把 playerId 编码进路径（简单替换空格，真实用 URL 编码更好）
    std::wstring path = L"/api/playtime/" + playerId;
    std::string resp = HttpJson(L"GET", path, playerId, "");
    if (resp.empty()) return -1;
    // 简单解析 total_seconds 字段（避免引入 JSON 库）
    size_t pos = resp.find("\"total_seconds\"");
    if (pos == std::string::npos) return -1;
    size_t colon = resp.find(':', pos);
    size_t comma = resp.find_first_of(",}", colon);
    if (colon == std::string::npos) return -1;
    std::string num = resp.substr(colon + 1, comma - colon - 1);
    try { return std::stoll(num); } catch (...) { return -1; }
}

long long CloudSync::UploadPlaytime(const std::wstring& playerId,
                                    long long sessionSeconds,
                                    long long clientTotalSeconds) {
    std::wstring path = L"/api/playtime/" + playerId;
    std::string body = "{\"session_seconds\":" + std::to_string(sessionSeconds) +
                       ",\"client_total_seconds\":" + std::to_string(clientTotalSeconds) + "}";
    std::string resp = HttpJson(L"POST", path, playerId, body);
    if (resp.empty()) return -1;
    size_t pos = resp.find("\"total_seconds\"");
    if (pos == std::string::npos) return -1;
    size_t colon = resp.find(':', pos);
    size_t comma = resp.find_first_of(",}", colon);
    if (colon == std::string::npos) return -1;
    std::string num = resp.substr(colon + 1, comma - colon - 1);
    try { return std::stoll(num); } catch (...) { return -1; }
}
