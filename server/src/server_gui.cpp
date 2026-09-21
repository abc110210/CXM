// DiskStress 服务端控制中心（自绘 UI，双面板布局）
//   左面板【实时数据】：设备卡片，绿点在线/灰点离线，实时 IOPS（1 秒刷新）
//   右面板【配置下发】：线程 / 队列深度 / 块大小 / IOPS 限制，下发后客户端立即生效，
//                       客户端随后的 STATS 会把新配置下的最新数据刷回左面板
// 协议（一行一条，\r\n 结尾）：
//   上行: HELLO|hostname|pid|version   STATS|iops|mbps|writes|errors|uptime
//   下行: CFG|threads|qd|block|iopsLimit
// 编译：server/build_server.bat（/MT 静态链接）

#define _CRT_SECURE_NO_WARNINGS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

#define DS_PORT        5757
#define OFFLINE_AFTER  8000     // ms 无心跳判定离线

// ------------------------------------------------------------ 调色板
static const COLORREF C_BG       = RGB(15, 18, 25);
static const COLORREF C_PANEL    = RGB(20, 25, 34);
static const COLORREF C_CARD     = RGB(28, 35, 47);
static const COLORREF C_CARD_SEL = RGB(33, 45, 62);
static const COLORREF C_BORDER   = RGB(50, 60, 76);
static const COLORREF C_ACCENT   = RGB(76, 194, 255);
static const COLORREF C_GREEN    = RGB(53, 208, 127);
static const COLORREF C_OFFLINE  = RGB(108, 116, 130);
static const COLORREF C_TEXT     = RGB(232, 236, 244);
static const COLORREF C_SUB      = RGB(140, 150, 166);
static const COLORREF C_DIM      = RGB(92, 100, 114);

// ------------------------------------------------------------ 数据
struct CfgValsS {
    uint32_t threads, qd, block, iops;
    CfgValsS() : threads(0), qd(0), block(0), iops(0) {}
};

struct Client {
    CRITICAL_SECTION lock;
    bool     lockInit;
    SOCKET   sock;
    std::string id;
    std::string ip;
    uint32_t pid;
    std::string ver;
    double   iops, mbps;
    uint64_t writes, errors, uptime;
    CfgValsS cfg;
    uint64_t lastSeen;
    bool     online;

    Client() : lockInit(false), sock(INVALID_SOCKET), pid(0), iops(0), mbps(0),
               writes(0), errors(0), uptime(0), lastSeen(0), online(false) {}
};

static std::vector<Client*> g_clients;
static CRITICAL_SECTION     g_clientsLock;
static bool                 g_clientsInit = false;

static wchar_t g_status[256] = L"就绪";
static int     g_selected = -1;

static HWND g_hwnd;
static HWND g_edThreads, g_edQd, g_edBlock, g_edIops;
static HWND g_btnOne, g_btnAll;
static HFONT g_fTitle, g_fPanel, g_fCard, g_fBody, g_fBig, g_fSmall;

static void ClientsInit() {
    if (!g_clientsInit) { InitializeCriticalSection(&g_clientsLock); g_clientsInit = true; }
}

static Client* FindById(const std::string& id) {
    for (size_t i = 0; i < g_clients.size(); i++)
        if (g_clients[i]->id == id) return g_clients[i];
    return NULL;
}

// ------------------------------------------------------------ 布局（双面板）
static RECT PanelLeft(const RECT& rc) {
    RECT r = {12, 76, rc.right - 336, rc.bottom - 12};
    if (r.right < r.left + 200) r.right = r.left + 200;
    return r;
}
static RECT PanelRight(const RECT& rc) {
    RECT r = {rc.right - 324, 76, rc.right - 12, rc.bottom - 12};
    if (r.right < r.left + 300) r.right = r.left + 300;
    return r;
}

// ------------------------------------------------------------ 网络
static bool SendLine(Client* c, const std::string& line) {
    EnterCriticalSection(&c->lock);
    SOCKET s = c->sock;
    LeaveCriticalSection(&c->lock);
    if (s == INVALID_SOCKET) return false;
    int off = 0;
    while (off < (int)line.size()) {
        int n = send(s, line.data() + off, (int)line.size() - off, 0);
        if (n == SOCKET_ERROR) return false;
        off += n;
    }
    return true;
}

struct SessArg { SOCKET sock; std::string ip; };

static DWORD WINAPI SessionThread(LPVOID p) {
    SessArg* a = (SessArg*)p;
    SOCKET s = a->sock;
    std::string ip = a->ip;
    std::string rbuf;
    Client* me = NULL;
    bool registered = false;

    uint64_t deadline = GetTickCount64() + 5000;
    while (GetTickCount64() < deadline) {
        fd_set rs; FD_ZERO(&rs); FD_SET(s, &rs);
        timeval tv = {0, 200 * 1000};
        int r = select(0, &rs, NULL, NULL, &tv);
        if (r <= 0) continue;
        char buf[2048];
        int n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0) { delete a; return 0; }
        rbuf.append(buf, n);
        if (rbuf.size() > 1024 * 1024) { delete a; return 0; }   // 无换行超 1 MiB 视为异常连接
        size_t pos = rbuf.find('\n');
        if (pos == std::string::npos) continue;
        std::string line = rbuf.substr(0, pos);
        rbuf.erase(0, pos + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();   // 兼容 \r\n
        if (line.rfind("HELLO|", 0) != 0) { delete a; return 0; }

        std::vector<std::string> f;
        size_t st = 0;
        while (true) {
            size_t nx = line.find('|', st);
            if (nx == std::string::npos) { f.push_back(line.substr(st)); break; }
            f.push_back(line.substr(st, nx - st));
            st = nx + 1;
        }
        if (f.size() < 4) { delete a; return 0; }

        ClientsInit();
        EnterCriticalSection(&g_clientsLock);
        me = FindById(f[1]);
        if (!me) {
            me = new Client();
            InitializeCriticalSection(&me->lock);
            me->id = f[1];
            g_clients.push_back(me);
        }
        EnterCriticalSection(&me->lock);
        if (me->sock != INVALID_SOCKET && me->sock != s) closesocket(me->sock);
        me->sock = s;
        me->ip = ip;
        me->pid = (uint32_t)atoi(f[2].c_str());
        me->ver = f[3];
        me->online = true;
        me->lastSeen = GetTickCount64();
        LeaveCriticalSection(&me->lock);
        LeaveCriticalSection(&g_clientsLock);
        registered = true;
        InvalidateRect(g_hwnd, NULL, FALSE);
        break;
    }
    if (!registered) { closesocket(s); delete a; return 0; }

    while (true) {
        fd_set rs; FD_ZERO(&rs); FD_SET(s, &rs);
        timeval tv = {0, 300 * 1000};
        int r = select(0, &rs, NULL, NULL, &tv);
        if (r > 0) {
            char buf[4096];
            int n = recv(s, buf, sizeof(buf), 0);
            if (n <= 0) break;
            rbuf.append(buf, n);
            if (rbuf.size() > 1024 * 1024) break;   // 防止无换行数据撑爆内存
            size_t pos;
            while ((pos = rbuf.find('\n')) != std::string::npos) {
                std::string line = rbuf.substr(0, pos);
                rbuf.erase(0, pos + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();   // 兼容 \r\n
                if (line.rfind("STATS|", 0) != 0) continue;
                std::vector<std::string> f;
                size_t st = 0;
                while (true) {
                    size_t nx = line.find('|', st);
                    if (nx == std::string::npos) { f.push_back(line.substr(st)); break; }
                    f.push_back(line.substr(st, nx - st));
                    st = nx + 1;
                }
                if (f.size() < 6) continue;
                EnterCriticalSection(&me->lock);
                me->iops    = atof(f[1].c_str());
                me->mbps    = atof(f[2].c_str());
                me->writes  = _strtoui64(f[3].c_str(), NULL, 10);
                me->errors  = _strtoui64(f[4].c_str(), NULL, 10);
                me->uptime  = _strtoui64(f[5].c_str(), NULL, 10);
                me->lastSeen = GetTickCount64();
                me->online  = true;
                LeaveCriticalSection(&me->lock);
                InvalidateRect(g_hwnd, NULL, FALSE);
            }
        } else if (r == SOCKET_ERROR) {
            break;
        }
        if (me) {
            EnterCriticalSection(&me->lock);
            bool alive = (GetTickCount64() - me->lastSeen) < OFFLINE_AFTER;
            LeaveCriticalSection(&me->lock);
            if (!alive) break;
        }
    }

    closesocket(s);
    if (me) {
        EnterCriticalSection(&me->lock);
        if (me->sock == s) {                 // 只有自己仍是注册连接时才判离线
            me->sock = INVALID_SOCKET;
            me->online = false;              // 否则说明已被新连接顶替，保持在线状态
        }
        LeaveCriticalSection(&me->lock);
    }
    InvalidateRect(g_hwnd, NULL, FALSE);
    delete a;
    return 0;
}

static DWORD WINAPI ListenThread(LPVOID) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;

    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET) return 1;
    sockaddr_in sa;
    ZeroMemory(&sa, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(DS_PORT);
    sa.sin_addr.s_addr = INADDR_ANY;
    int yes = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (char*)&yes, sizeof(yes));
    if (bind(ls, (sockaddr*)&sa, sizeof(sa)) != 0 || listen(ls, 16) != 0) {
        swprintf(g_status, 256, L"监听失败（端口 %d 被占用？）err=%d", DS_PORT, WSAGetLastError());
        InvalidateRect(g_hwnd, NULL, FALSE);
        return 1;
    }
    swprintf(g_status, 256, L"监听中 0.0.0.0:%d", DS_PORT);
    InvalidateRect(g_hwnd, NULL, FALSE);

    while (true) {
        sockaddr_in cli;
        int cl = sizeof(cli);
        SOCKET s = accept(ls, (sockaddr*)&cli, &cl);
        if (s == INVALID_SOCKET) break;
        char ip[64];
        sprintf(ip, "%u.%u.%u.%u",
                cli.sin_addr.S_un.S_un_b.s_b1, cli.sin_addr.S_un.S_un_b.s_b2,
                cli.sin_addr.S_un.S_un_b.s_b3, cli.sin_addr.S_un.S_un_b.s_b4);
        SessArg* a = new SessArg();
        a->sock = s;
        a->ip = ip;
        CreateThread(NULL, 0, SessionThread, a, 0, NULL);
    }
    return 0;
}

// ------------------------------------------------------------ 下发
static void PushConfig(bool toAll) {
    if (!toAll && g_selected < 0) {
        wcscpy(g_status, L"请先点击左侧卡片选中一台设备，或使用“下发到全部在线”");
        InvalidateRect(g_hwnd, NULL, FALSE);
        return;
    }
    wchar_t wt[16], wq[16], wb[16], wi[16];
    GetWindowTextW(g_edThreads, wt, 16);
    GetWindowTextW(g_edQd, wq, 16);
    GetWindowTextW(g_edBlock, wb, 16);
    GetWindowTextW(g_edIops, wi, 16);

    uint32_t th = (uint32_t)_wtoi(wt);
    uint32_t qd = (uint32_t)_wtoi(wq);
    uint32_t bl = (uint32_t)_wtoi(wb);
    uint32_t io = (uint32_t)_wtoi(wi);
    if (th < 1 || th > 64)  { wcscpy(g_status, L"线程数需在 1-64");   InvalidateRect(g_hwnd, NULL, FALSE); return; }
    if (qd < 1 || qd > 128) { wcscpy(g_status, L"队列深度需在 1-128"); InvalidateRect(g_hwnd, NULL, FALSE); return; }
    if (bl < 512 || bl > 1048576 || (bl % 512) != 0) {
        wcscpy(g_status, L"块大小需为 512 的倍数（512 B - 1 MiB）");
        InvalidateRect(g_hwnd, NULL, FALSE);
        return;
    }

    char line[256];
    sprintf(line, "CFG|%u|%u|%u|%u\r\n", th, qd, bl, io);

    int sent = 0, tried = 0;
    EnterCriticalSection(&g_clientsLock);
    for (size_t i = 0; i < g_clients.size(); i++) {
        Client* c = g_clients[i];
        bool target;
        EnterCriticalSection(&c->lock);
        target = c->online && (toAll || (int)i == g_selected);
        LeaveCriticalSection(&c->lock);
        if (!target) continue;
        tried++;
        if (SendLine(c, line)) {
            sent++;
            EnterCriticalSection(&c->lock);
            c->cfg.threads = th; c->cfg.qd = qd; c->cfg.block = bl; c->cfg.iops = io;
            LeaveCriticalSection(&c->lock);
        }
    }
    LeaveCriticalSection(&g_clientsLock);

    swprintf(g_status, 256, L"下发%s：成功 %d / 在线 %d，客户端生效后 2 秒内数据自动刷新",
             toAll ? L"全部在线" : L"选中设备", sent, tried);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

// ------------------------------------------------------------ 绘制辅助
static void FillRound(HDC dc, RECT rc, int rad, COLORREF fill, COLORREF border) {
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = CreatePen(PS_SOLID, 1, border);
    HBRUSH ob = (HBRUSH)SelectObject(dc, b);
    HPEN op = (HPEN)SelectObject(dc, p);
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, rad, rad);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(p);
}

static void DrawTextAt(HDC dc, const wchar_t* t, int x, int y, HFONT f, COLORREF c) {
    HFONT of = (HFONT)SelectObject(dc, f);
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    TextOutW(dc, x, y, t, (int)wcslen(t));
    SelectObject(dc, of);
}

static void DrawDot(HDC dc, int cx, int cy, int r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    HPEN p = CreatePen(PS_SOLID, 1, c);
    HBRUSH ob = (HBRUSH)SelectObject(dc, b);
    HPEN op = (HPEN)SelectObject(dc, p);
    Ellipse(dc, cx - r, cy - r, cx + r, cy + r);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(p);
}

static std::wstring BlockToStr(uint32_t b) {
    wchar_t t[32];
    if (b >= 1024) swprintf(t, 32, L"%uK", b / 1024);
    else swprintf(t, 32, L"%uB", b);
    return std::wstring(t);
}

// ------------------------------------------------------------ 消息处理
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_fTitle = CreateFontW(-24, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH, L"Segoe UI");
        g_fPanel = CreateFontW(-17, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH, L"Segoe UI");
        g_fCard  = CreateFontW(-16, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH, L"Segoe UI");
        g_fBody  = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH, L"Segoe UI");
        g_fBig   = CreateFontW(-26, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH, L"Segoe UI");
        g_fSmall = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH, L"Segoe UI");

        DWORD style = WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER;
        g_edThreads = CreateWindowW(L"EDIT", L"4",    style, 0, 0, 100, 26, hwnd, (HMENU)101, NULL, NULL);
        g_edQd      = CreateWindowW(L"EDIT", L"8",    style, 0, 0, 100, 26, hwnd, (HMENU)102, NULL, NULL);
        g_edBlock   = CreateWindowW(L"EDIT", L"4096", style, 0, 0, 100, 26, hwnd, (HMENU)103, NULL, NULL);
        g_edIops    = CreateWindowW(L"EDIT", L"0",    style, 0, 0, 100, 26, hwnd, (HMENU)104, NULL, NULL);
        g_btnOne = CreateWindowW(L"BUTTON", L"下发到选中",
                                 WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                 0, 0, 100, 32, hwnd, (HMENU)201, NULL, NULL);
        g_btnAll = CreateWindowW(L"BUTTON", L"下发到全部在线",
                                 WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                 0, 0, 100, 32, hwnd, (HMENU)202, NULL, NULL);
        SendMessageW(g_edThreads, WM_SETFONT, (WPARAM)g_fBody, TRUE);
        SendMessageW(g_edQd, WM_SETFONT, (WPARAM)g_fBody, TRUE);
        SendMessageW(g_edBlock, WM_SETFONT, (WPARAM)g_fBody, TRUE);
        SendMessageW(g_edIops, WM_SETFONT, (WPARAM)g_fBody, TRUE);
        SendMessageW(g_btnOne, WM_SETFONT, (WPARAM)g_fBody, TRUE);
        SendMessageW(g_btnAll, WM_SETFONT, (WPARAM)g_fBody, TRUE);
        SetTimer(hwnd, 1, 1000, NULL);
        CreateThread(NULL, 0, ListenThread, NULL, 0, NULL);
        return 0;
    }
    case WM_TIMER:
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_COMMAND:
        if (HIWORD(wp) == BN_CLICKED) {
            if (LOWORD(wp) == 201) PushConfig(false);
            if (LOWORD(wp) == 202) PushConfig(true);
        }
        return 0;
    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        RECT rc; GetClientRect(hwnd, &rc);
        RECT pl = PanelLeft(rc);
        int cardW = pl.right - pl.left - 24, cardH = 118;
        int hidden = 0;
        EnterCriticalSection(&g_clientsLock);
        for (int i = 0; i < (int)g_clients.size(); i++) {
            Client* c = g_clients[i];
            int cy = pl.top + 48 + i * (cardH + 12);
            if (cy + cardH > pl.bottom - 8) { hidden++; continue; }
            EnterCriticalSection(&c->lock);
            RECT card = {pl.left + 12, cy, pl.left + 12 + cardW, cy + cardH};
            bool sel = (i == g_selected);
            FillRound(mem, card, 10, sel ? C_CARD_SEL : C_CARD, sel ? C_ACCENT : C_BORDER);

            DrawDot(mem, card.left + 20, card.top + 22, 6, c->online ? C_GREEN : C_OFFLINE);
            wchar_t hostW[128];
            MultiByteToWideChar(CP_UTF8, 0, c->id.c_str(), -1, hostW, 128);
            DrawTextAt(mem, hostW, card.left + 34, card.top + 10, g_fCard,
                       c->online ? C_TEXT : C_OFFLINE);

            wchar_t ipw[64], verw[32];
            MultiByteToWideChar(CP_UTF8, 0, c->ip.c_str(), -1, ipw, 64);
            MultiByteToWideChar(CP_UTF8, 0, c->ver.c_str(), -1, verw, 32);
            swprintf(tmp, 256, L"%s    PID %u    v%s    %s", ipw, c->pid, verw,
                     c->online ? L"在线" : L"离线");
            DrawTextAt(mem, tmp, card.left + 34, card.top + 36, g_fBody, C_SUB);

            HPEN dpen = CreatePen(PS_SOLID, 1, C_BORDER);
            HPEN dop = (HPEN)SelectObject(mem, dpen);
            MoveToEx(mem, card.left + 16, card.top + 60, NULL);
            LineTo(mem, card.right - 16, card.top + 60);
            SelectObject(mem, dop);
            DeleteObject(dpen);

            swprintf(tmp, 256, L"写入 %llu    错误 %llu    运行 %llu 分 %llu 秒",
                     (unsigned long long)c->writes, (unsigned long long)c->errors,
                     c->uptime / 60, c->uptime % 60);
            DrawTextAt(mem, tmp, card.left + 20, card.top + 68, g_fBody, C_TEXT);

            {
                std::vector<std::wstring> chips;
                chips.push_back(L"线程 " + std::to_wstring(c->cfg.threads));
                chips.push_back(L"QD " + std::to_wstring(c->cfg.qd));
                chips.push_back(L"块 " + BlockToStr(c->cfg.block));
                chips.push_back(c->cfg.iops == 0 ? std::wstring(L"IOPS 不限")
                                                 : (L"IOPS ≤ " + std::to_wstring(c->cfg.iops)));
                int cx = card.left + 20;
                for (size_t k = 0; k < chips.size(); k++) {
                    DrawTextAt(mem, chips[k].c_str(), cx, card.top + 94, g_fBody, C_TEXT);
                    SIZE csz;
                    HFONT ofc = (HFONT)SelectObject(mem, g_fBody);
                    GetTextExtentPoint32W(mem, chips[k].c_str(), (int)chips[k].size(), &csz);
                    SelectObject(mem, ofc);
                    cx += csz.cx + 8;
                    if (k + 1 < chips.size() && cx + 12 < card.right - 16) {
                        DrawTextAt(mem, L"|", cx, card.top + 94, g_fBody, C_BORDER);
                        cx += 16;
                    }
                }
            }

            if (c->online) {
                wchar_t big[64];
                if (c->iops >= 1000) swprintf(big, 64, L"%.1fK", c->iops / 1000.0);
                else swprintf(big, 64, L"%.0f", c->iops);
                HFONT ofb = (HFONT)SelectObject(mem, g_fBig);
                SIZE isz; GetTextExtentPoint32W(mem, big, (int)wcslen(big), &isz);
                SelectObject(mem, ofb);
                DrawTextAt(mem, big, card.right - isz.cx - 18, card.top + 10, g_fBig, C_ACCENT);
                DrawTextAt(mem, L"IOPS", card.right - 54, card.top + 44, g_fSmall, C_SUB);
                swprintf(tmp, 256, L"%.1f MiB/s", c->mbps);
                DrawTextAt(mem, tmp, card.right - 112, card.top + 62, g_fSmall, C_SUB);
            }
            LeaveCriticalSection(&c->lock);
        }
        LeaveCriticalSection(&g_clientsLock);
        g_selected = -1;
        for (int i = 0; i < n; i++) {
            int cy = pl.top + 48 + i * (cardH + 10);
            if (cy + cardH > pl.bottom - 8) break;
            if (x >= pl.left + 12 && x <= pl.left + 12 + cardW &&
                y >= cy && y <= cy + cardH) { g_selected = i; break; }
        }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_SIZE: {
        RECT rc; GetClientRect(hwnd, &rc);
        RECT pr = PanelRight(rc);
        int x = pr.left + 24, w = pr.right - pr.left - 48;
        MoveWindow(g_edThreads, x, pr.top + 68,  w, 26, TRUE);
        MoveWindow(g_edQd,      x, pr.top + 126, w, 26, TRUE);
        MoveWindow(g_edBlock,   x, pr.top + 184, w, 26, TRUE);
        MoveWindow(g_edIops,    x, pr.top + 242, w, 26, TRUE);
        MoveWindow(g_btnOne,    x, pr.top + 292, w, 32, TRUE);
        MoveWindow(g_btnAll,    x, pr.top + 334, w, 32, TRUE);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);

        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HBITMAP ob = (HBITMAP)SelectObject(mem, bmp);

        HBRUSH bg = CreateSolidBrush(C_BG);
        FillRect(mem, &rc, bg);
        DeleteObject(bg);

        wchar_t tmp[512];

        // ---- header ----
        DrawTextAt(mem, L"DiskStress 控制中心", 20, 16, g_fTitle, C_TEXT);
        EnterCriticalSection(&g_clientsLock);
        int total = (int)g_clients.size(), online = 0;
        for (int i = 0; i < total; i++) {
            EnterCriticalSection(&g_clients[i]->lock);
            if (g_clients[i]->online) online++;
            LeaveCriticalSection(&g_clients[i]->lock);
        }
        LeaveCriticalSection(&g_clientsLock);
        swprintf(tmp, 256, L"在线 %d / 共 %d     %s", online, total, g_status);
        HFONT ofm = (HFONT)SelectObject(mem, g_fBody);
        SIZE sz; GetTextExtentPoint32W(mem, tmp, (int)wcslen(tmp), &sz);
        SelectObject(mem, ofm);
        DrawTextAt(mem, tmp, rc.right - sz.cx - 20, 26, g_fBody, C_SUB);

        // ---- 左面板【实时数据】 ----
        RECT pl = PanelLeft(rc);
        FillRound(mem, pl, 12, C_PANEL, C_BORDER);
        DrawTextAt(mem, L"实时数据", pl.left + 18, pl.top + 12, g_fPanel, C_TEXT);
        DrawTextAt(mem, L"每秒刷新 · 绿点在线 / 灰点离线 · 点击卡片选中",
                   pl.left + 130, pl.top + 16, g_fSmall, C_SUB);

        int cardW = pl.right - pl.left - 24, cardH = 104;
        int hidden = 0;
        EnterCriticalSection(&g_clientsLock);
        for (int i = 0; i < (int)g_clients.size(); i++) {
            Client* c = g_clients[i];
            int cy = pl.top + 48 + i * (cardH + 10);
            if (cy + cardH > pl.bottom - 8) { hidden++; continue; }
            EnterCriticalSection(&c->lock);
            RECT card = {pl.left + 12, cy, pl.left + 12 + cardW, cy + cardH};
            bool sel = (i == g_selected);
            FillRound(mem, card, 10, sel ? C_CARD_SEL : C_CARD, sel ? C_ACCENT : C_BORDER);

            DrawDot(mem, card.left + 20, card.top + 24, 6, c->online ? C_GREEN : C_OFFLINE);
            wchar_t hostW[128];
            MultiByteToWideChar(CP_UTF8, 0, c->id.c_str(), -1, hostW, 128);
            DrawTextAt(mem, hostW, card.left + 34, card.top + 12, g_fCard,
                       c->online ? C_TEXT : C_OFFLINE);

            wchar_t ipw[64], verw[32];
            MultiByteToWideChar(CP_UTF8, 0, c->ip.c_str(), -1, ipw, 64);
            MultiByteToWideChar(CP_UTF8, 0, c->ver.c_str(), -1, verw, 32);
            swprintf(tmp, 256, L"%s   PID %u   v%s   %s", ipw, c->pid, verw,
                     c->online ? L"在线" : L"离线");
            DrawTextAt(mem, tmp, card.left + 34, card.top + 38, g_fBody, C_SUB);

            swprintf(tmp, 256, L"写入 %llu    错误 %llu    运行 %llu 分 %llu 秒",
                     (unsigned long long)c->writes, (unsigned long long)c->errors,
                     c->uptime / 60, c->uptime % 60);
            DrawTextAt(mem, tmp, card.left + 20, card.top + 66, g_fBody, C_TEXT);

            swprintf(tmp, 256, L"线程 %u x QD %u · 块 %u B · IOPS %s%u",
                     c->cfg.threads, c->cfg.qd, c->cfg.block,
                     c->cfg.iops == 0 ? L"不限 " : L"≤ ", c->cfg.iops);
            DrawTextAt(mem, tmp, card.left + 20, card.top + 88, g_fSmall, C_SUB);

            if (c->online) {
                wchar_t big[64];
                if (c->iops >= 1000) swprintf(big, 64, L"%.1fK", c->iops / 1000.0);
                else swprintf(big, 64, L"%.0f", c->iops);
                HFONT ofb = (HFONT)SelectObject(mem, g_fBig);
                SIZE isz; GetTextExtentPoint32W(mem, big, (int)wcslen(big), &isz);
                SelectObject(mem, ofb);
                DrawTextAt(mem, big, card.right - isz.cx - 16, card.top + 14, g_fBig, C_ACCENT);
                DrawTextAt(mem, L"IOPS", card.right - 56, card.top + 46, g_fSmall, C_SUB);
                swprintf(tmp, 256, L"%.1f MiB/s", c->mbps);
                DrawTextAt(mem, tmp, card.right - 110, card.top + 62, g_fSmall, C_SUB);
            }
            LeaveCriticalSection(&c->lock);
        }
        LeaveCriticalSection(&g_clientsLock);
        if (hidden > 0) {
            swprintf(tmp, 256, L"... 还有 %d 台未显示", hidden);
            DrawTextAt(mem, tmp, pl.left + 18, pl.bottom - 26, g_fSmall, C_DIM);
        }
        if (g_clients.empty()) {
            DrawTextAt(mem, L"等待客户端接入 ...（客户端自动连接本机 5757 端口）",
                       pl.left + 18, pl.top + 56, g_fBody, C_SUB);
        }

        // ---- 右面板【配置下发】 ----
        RECT pr = PanelRight(rc);
        FillRound(mem, pr, 12, C_PANEL, C_ACCENT);
        DrawTextAt(mem, L"配置下发", pr.left + 18, pr.top + 12, g_fPanel, C_TEXT);
        DrawTextAt(mem, L"下发后立即生效", pr.right - 122, pr.top + 16, g_fSmall, C_SUB);

        int lx = pr.left + 24;
        DrawTextAt(mem, L"IO 并发线程数（1-64）",        lx, pr.top + 46,  g_fSmall, C_SUB);
        DrawTextAt(mem, L"队列深度 QD（1-128）",         lx, pr.top + 104, g_fSmall, C_SUB);
        DrawTextAt(mem, L"块大小（512 的倍数，B）",      lx, pr.top + 162, g_fSmall, C_SUB);
        DrawTextAt(mem, L"IOPS 限制（0 = 不限）",        lx, pr.top + 220, g_fSmall, C_SUB);
        DrawTextAt(mem, L"下发后：客户端重建线程池（约 0.5 秒），",
                   lx, pr.bottom - 44, g_fSmall, C_DIM);
        DrawTextAt(mem, L"新配置下的最新数据 2 秒内刷回左侧实时数据。",
                   lx, pr.bottom - 26, g_fSmall, C_DIM);

        // ---- present ----
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, ob);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    ClientsInit();

    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"DiskStressServerWnd";
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    if (!RegisterClassW(&wc)) return 1;

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"DiskStress 服务端控制中心",
                                WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 1080, 720,
                                NULL, NULL, hInst, NULL);
    if (!hwnd) return 1;
    g_hwnd = hwnd;
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    return 0;
}
