// OAuth2 实现
#include "oauth.h"
#include "common.h"
#include <winhttp.h>
#include <ctime>

#pragma comment(lib, "winhttp.lib")

OAuth::OAuth(const Config& cfg)
    : clientId_(cfg.clientId), clientSecret_(cfg.clientSecret), redirectUri_(cfg.redirectUri) {}

std::wstring OAuth::BuildAuthorizeUrl() const {
    // 标准授权码流程授权地址
    return L"https://littleskin.cn/oauth/authorize"
           L"?client_id=" + clientId_
           + L"&redirect_uri=" + redirectUri_
           + L"&response_type=code"
           + L"&scope=minecraft";
}

// 内部：POST 表单到 https://littleskin.cn/oauth/token
static std::string PostToken(const std::wstring& body) {
    HINTERNET hS = WinHttpOpen(L"StardustLauncher/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hS) return {};
    HINTERNET hC = WinHttpConnect(hS, L"littleskin.cn", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hC) { WinHttpCloseHandle(hS); return {}; }
    HINTERNET hR = WinHttpOpenRequest(hC, L"POST", L"/oauth/token", nullptr,
                                      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                      WINHTTP_FLAG_SECURE);
    if (!hR) { WinHttpCloseHandle(hC); WinHttpCloseHandle(hS); return {}; }

    std::wstring headers = L"Content-Type: application/x-www-form-urlencoded\r\n";
    std::string bodyUtf8 = util::WStringToString(body);
    if (!WinHttpSendRequest(hR, headers.c_str(), (DWORD)-1L,
                            (LPVOID)bodyUtf8.data(), (DWORD)bodyUtf8.size(),
                            (DWORD)bodyUtf8.size(), 0)) {
        WinHttpCloseHandle(hR); WinHttpCloseHandle(hC); WinHttpCloseHandle(hS); return {};
    }
    if (!WinHttpReceiveResponse(hR, nullptr)) {
        WinHttpCloseHandle(hR); WinHttpCloseHandle(hC); WinHttpCloseHandle(hS); return {};
    }
    std::string resp; DWORD avail = 0, read = 0;
    do {
        avail = 0;
        if (!WinHttpQueryDataAvailable(hR, &avail)) break;
        if (!avail) break;
        std::vector<char> buf(avail + 1, 0);
        if (!WinHttpReadData(hR, buf.data(), avail, &read)) break;
        resp.append(buf.data(), read);
    } while (avail > 0);
    WinHttpCloseHandle(hR); WinHttpCloseHandle(hC); WinHttpCloseHandle(hS);
    return resp;
}

// 从 JSON 里取某个字符串字段的值
static std::string ExtractString(const std::string& json, const std::string& key) {
    std::string target = "\"" + key + "\"";
    size_t pos = json.find(target);
    if (pos == std::string::npos) return {};
    size_t colon = json.find(':', pos);
    if (colon == std::string::npos) return {};
    size_t q1 = json.find('"', colon + 1);
    if (q1 == std::string::npos) return {};
    size_t q2 = json.find('"', q1 + 1);
    if (q2 == std::string::npos) return {};
    return json.substr(q1 + 1, q2 - q1 - 1);
}

bool OAuth::ExchangeCode(const std::wstring& code, Config& cfg) {
    std::wstring body = L"grant_type=authorization_code"
                        L"&client_id=" + clientId_
                        + L"&client_secret=" + clientSecret_
                        + L"&code=" + code
                        + L"&redirect_uri=" + redirectUri_;
    std::string resp = PostToken(body);
    if (resp.empty()) return false;
    cfg.accessToken = util::StringToWString(ExtractString(resp, "access_token"));
    cfg.refreshToken = util::StringToWString(ExtractString(resp, "refresh_token"));
    return !cfg.accessToken.empty();
}

bool OAuth::Refresh(Config& cfg) {
    if (cfg.refreshToken.empty()) return false;
    std::wstring body = L"grant_type=refresh_token"
                        L"&client_id=" + clientId_
                        + L"&client_secret=" + clientSecret_
                        + L"&refresh_token=" + cfg.refreshToken;
    std::string resp = PostToken(body);
    if (resp.empty()) return false;
    cfg.accessToken = util::StringToWString(ExtractString(resp, "access_token"));
    if (!cfg.accessToken.empty()) return true;
    return false;
}
