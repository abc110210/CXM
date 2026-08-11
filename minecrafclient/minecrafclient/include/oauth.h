#pragma once
// LittleSkin OAuth2 授权码流程：构造授权 URL + 用 code 换 token
#include <string>
#include "config.h"

class OAuth {
public:
    OAuth(const Config& cfg);

    // 构造授权页面 URL（WebView2 打开它，用户登录授权）
    std::wstring BuildAuthorizeUrl() const;

    // 用授权回调拿到的 code 换取 access_token / refresh_token
    // 成功返回 true，并把 token 写入 cfg
    bool ExchangeCode(const std::wstring& code, Config& cfg);

    // 用 refresh_token 刷新 access_token
    bool Refresh(Config& cfg);

private:
    std::wstring clientId_;
    std::wstring clientSecret_;
    std::wstring redirectUri_;
};
