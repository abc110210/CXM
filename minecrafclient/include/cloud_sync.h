#pragma once
// 云端时长同步：HMAC-SHA256 签名 + WinHTTP HTTPS 请求
// 鉴权：请求头 X-Player-ID / X-Timestamp / X-Token
//       Token = HMAC-SHA256(secret, "player_id=ID&ts=时间戳")
#include <string>
#include <windows.h>

class CloudSync {
public:
    CloudSync(const std::wstring& serverUrl, const std::wstring& secret);

    // 拉取累计游玩时长（秒），失败返回 -1
    long long GetPlaytime(const std::wstring& playerId);

    // 上传时长，成功返回服务端累计秒数，失败返回 -1
    // sessionSeconds: 本次新增秒数；clientTotalSeconds: 客户端本地总秒数
    long long UploadPlaytime(const std::wstring& playerId,
                             long long sessionSeconds,
                             long long clientTotalSeconds);

    // 直接计算签名（供调试/校验）
    std::string MakeToken(const std::wstring& playerId, long long timestamp);

private:
    std::wstring serverUrl_;
    std::wstring secret_;

    // 内部：发送 JSON 请求，返回响应文本；出错返回空串
    std::string HttpJson(const std::wstring& method,
                         const std::wstring& path,
                         const std::wstring& playerId,
                         const std::string& bodyJson);
};
