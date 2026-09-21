// 客户端网络线程：TCP 连接服务器，断线自动重连
//   上行: HELLO|hostname|pid|version / STATS|iops|mbps|writes|errors|uptime
//   下行: CFG|threads|qd|block|iopsLimit
#define _CRT_SECURE_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include "net.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdio>
#include <string>

#pragma comment(lib, "ws2_32.lib")

namespace {

struct NetArg {
    HANDLE        hStop;
    LiveStatsFn   fn;
    std::wstring  stateDir;
};

std::string ToUtf8(const std::wstring& w) {
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    std::string s((size_t)len, '\0');
    if (len > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], len, NULL, NULL);
    return s;
}

bool SendAll(SOCKET s, const std::string& data) {
    int off = 0;
    while (off < (int)data.size()) {
        int n = send(s, data.data() + off, (int)data.size() - off, 0);
        if (n == SOCKET_ERROR) return false;
        off += n;
    }
    return true;
}

void ApplyCfgLine(const std::string& lineIn, const std::wstring& stateDir) {
    // CFG|threads|qd|block|iopsLimit
    std::string line = lineIn;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    int f[5] = {0, 0, 0, 0, 0};
    int fi = 0, val = 0;
    bool hasVal = false, ok = true;
    for (size_t i = 4; i < line.size() && ok; i++) {
        char ch = line[i];
        if (ch == '|') {
            if (!hasVal || fi >= 5) { ok = false; break; }
            f[fi++] = val; val = 0; hasVal = false;
        } else if (ch >= '0' && ch <= '9') {
            val = val * 10 + (ch - '0');
            if (val > 100000000) val = 100000000;
            hasVal = true;
        } else {
            ok = false;
        }
    }
    if (!ok || !hasVal || fi != 4) {
        AppendDebugLog(stateDir, L"[FAIL] 配置下发 | 无法解析: " +
                       std::wstring(line.begin(), line.end()));
        return;
    }

    CfgVals nv;
    nv.threads    = (uint32_t)f[0];
    nv.queueDepth = (uint32_t)f[1];
    nv.blockBytes = (uint32_t)f[2];
    nv.iopsLimit  = (uint32_t)f[3];

    // 合理范围
    if (nv.threads < 1 || nv.threads > 64)         return;
    if (nv.queueDepth < 1 || nv.queueDepth > 128)  return;
    if (nv.blockBytes < 512 || nv.blockBytes > 1024 * 1024 || (nv.blockBytes % 512) != 0) return;

    if (RtApply(&g_rt, nv)) {
        // 生效细节由监督循环记录（配置生效/重建线程池）
        wchar_t msg[256];
        swprintf(msg, 256, L"[OK]   配置下发 | 线程=%u QD=%u 块=%u IOPS限制=%u (版本 %llu)",
                 nv.threads, nv.queueDepth, nv.blockBytes, nv.iopsLimit,
                 (unsigned long long)RtGet(&g_rt).version);
        AppendDebugLog(stateDir, msg);
    }
}

DWORD WINAPI NetThread(LPVOID p) {
    NetArg* a = (NetArg*)p;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { delete a; return 1; }

    wchar_t whost[256] = {0};
    DWORD   wn = 256;
    GetComputerNameW(whost, &wn);
    std::string host = ToUtf8(whost);
    std::string ip   = ToUtf8(kServerIP);
    std::string ver  = ToUtf8(APP_VER);

    wchar_t pidw[16];
    swprintf(pidw, 16, L"%lu", GetCurrentProcessId());
    std::string pid = ToUtf8(pidw);

    AppendDebugLog(a->stateDir, L"[OK]   在线模式 | 目标 " + std::wstring(kServerIP) +
                   L":" + FormatInt(kServerPort) + L"，自动重连已启用");

    uint64_t prevWrites = 0;
    uint64_t prevTick   = 0;

    bool wasConnected = false;
    uint64_t lastFailLog = 0;

    while (WaitForSingleObject(a->hStop, 0) != WAIT_OBJECT_0) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) { Sleep(3000); continue; }

        sockaddr_in sa;
        ZeroMemory(&sa, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port   = htons(kServerPort);
        sa.sin_addr.s_addr = inet_addr(ip.c_str());
        if (sa.sin_addr.s_addr == INADDR_NONE) {
            hostent* he = gethostbyname(ip.c_str());
            if (!he || !he->h_addr_list[0]) { closesocket(s); Sleep(3000); continue; }
            CopyMemory(&sa.sin_addr, he->h_addr_list[0], 4);
        }

        uint64_t nowLog = GetTickCount64();
        bool logThis = wasConnected || (nowLog - lastFailLog) >= 60000;   // 离线时最多每分钟记一条
        if (logThis)
            AppendDebugLog(a->stateDir, L"[OK]   在线模式 | 正在连接服务器 " +
                           std::wstring(kServerIP) + L":" + FormatInt(kServerPort));
        if (connect(s, (sockaddr*)&sa, sizeof(sa)) != 0) {
            if (logThis) {
                AppendDebugLog(a->stateDir, L"[FAIL] 在线模式 | 连接失败 err=" +
                               FormatInt(WSAGetLastError()) + L"，3 秒后重试");
                lastFailLog = GetTickCount64();
            }
            wasConnected = false;
            closesocket(s);
            if (WaitForSingleObject(a->hStop, 3000) == WAIT_OBJECT_0) break;
            continue;
        }
        AppendDebugLog(a->stateDir, L"[OK]   在线模式 | 已连接服务器");
        wasConnected = true;

        // u_long mode = 0; ioctlsocket(s, FIONBIO, &mode); // blocking
        SendAll(s, "HELLO|" + host + "|" + pid + "|" + ver + "\r\n");

        std::string rbuf;
        bool broken = false;
        uint64_t lastSend = GetTickCount64();

        while (!broken) {
            if (WaitForSingleObject(a->hStop, 0) == WAIT_OBJECT_0) break;

            fd_set rs;
            FD_ZERO(&rs);
            FD_SET(s, &rs);
            timeval tv;
            tv.tv_sec  = 0;
            tv.tv_usec = 200 * 1000;
            int r = select(0, &rs, NULL, NULL, &tv);
            if (r > 0) {
                char buf[4096];
                int n = recv(s, buf, sizeof(buf), 0);
                if (n <= 0) { broken = true; break; }
                rbuf.append(buf, n);
                if (rbuf.size() > 1024 * 1024) { broken = true; break; }   // 异常数据保护
                size_t pos;
                while ((pos = rbuf.find('\n')) != std::string::npos) {
                    std::string line = rbuf.substr(0, pos);
                    rbuf.erase(0, pos + 1);
                    if (line.rfind("CFG|", 0) == 0) ApplyCfgLine(line, a->stateDir);
                }
            } else if (r == SOCKET_ERROR) {
                broken = true;
                break;
            }

            uint64_t now = GetTickCount64();
            if (now - lastSend >= 2000) {
                lastSend = now;
                LiveStats ls;
                a->fn(&ls);

                double dt = (double)(now - (prevTick ? prevTick : now)) / 1000.0;
                double iops = 0;
                if (prevTick && dt > 0.2) iops = (double)(ls.writes - prevWrites) / dt;
                prevWrites = ls.writes;
                prevTick   = now;

                char line[512];
                sprintf(line, "STATS|%.1f|%.2f|%llu|%llu|%llu\r\n",
                        iops, ls.mbps,
                        (unsigned long long)ls.writes,
                        (unsigned long long)ls.errors,
                        (unsigned long long)ls.uptimeSec);
                if (!SendAll(s, line)) { broken = true; break; }
            }
        }

        closesocket(s);
        AppendDebugLog(a->stateDir, L"[FAIL] 在线模式 | 连接断开，3 秒后重连");
        wasConnected = false;
        if (WaitForSingleObject(a->hStop, 3000) == WAIT_OBJECT_0) break;
    }

    WSACleanup();
    delete a;
    return 0;
}

} // namespace

void NetStartThread(HANDLE hStop, LiveStatsFn fn, const std::wstring& stateDir) {
    NetArg* a = new NetArg();
    a->hStop    = hStop;
    a->fn       = fn;
    a->stateDir = stateDir;
    HANDLE th = CreateThread(NULL, 0, NetThread, a, 0, NULL);
    if (th) CloseHandle(th);   // detached; exits on hStop
    else delete a;             // 线程创建失败时回收参数，避免泄漏
}
