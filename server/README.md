# DiskStress 服务端控制中心（你本地编译）

自绘 UI 的服务端：实时显示各客户端在线状态，可向在线客户端下发配置（线程数 / 队列深度 / 块大小 / IOPS 限制），下发后**立即生效**。

## 编译

**方式一：本地编译**

```bat
:: x64 Native Tools Command Prompt for VS 里执行
build_server.bat
```

产出 `out\DiskStressServer.exe`（/MT 静态链接，无运行库依赖）。

**方式二：GitHub Actions**

仓库根目录的 `.github/workflows/build-server.yml` 会在 `server/**` 变更时自动编译，
也可在 Actions 页面手动触发（Build DiskStressServer → Run workflow），
产物 `DiskStressServer` 在 job 底部 Artifacts 下载；打 `v*` tag 自动挂 Release。

## 使用

1. 双击运行，窗口顶部显示 `监听中 0.0.0.0:5757`
2. 客户端（DiskStressStart 服务）会**自动连接**本机 5757 端口并出现在卡片列表
3. 点击卡片选中设备，在底部填好配置，点 **下发到选中**（或 **下发到全部在线**）
4. 客户端收到后立即生效：IOPS 限制即时生效；线程/队列深度/块大小变化会触发线程池热重建（约 0.5 秒内）

## 界面说明（双面板布局）

- **左面板【实时数据】**：设备卡片列表，绿点在线 / 灰点离线，右侧大号实时 IOPS + MiB/s，
  卡片底部为该设备当前生效的配置；点击卡片选中（蓝色边框）
- **右面板【配置下发】**：4 个输入框 + 2 个按钮，与左侧明确分开
- 下发后客户端约 0.5 秒内生效，新数据 2 秒内自动刷回左面板

## 协议（TCP 5757，一行一条 \r\n）

| 方向 | 格式 |
|---|---|
| 客户端 → 服务端 | `HELLO\|hostname\|pid\|version` |
| 客户端 → 服务端 | `STATS\|iops\|mbps\|writes\|errors\|uptime`（每 2 秒） |
| 服务端 → 客户端 | `CFG\|threads\|qd\|block\|iopsLimit` |

客户端在 `start/src/shared.h` 里改 `kServerIP` 后重新编译即指向你的服务器。

## 注意

- 服务端与客户端之间若有防火墙，需放行 **TCP 5757**
- 客户端断线会自动每 3 秒重连，服务端 8 秒无心跳判离线；同名设备重连会顶掉旧连接（旧连接退出不会误标离线）
- 下发的新块大小若与磁盘扇区不兼容，客户端会自动回退缓存模式并在 debug.log 标注
- **协议无鉴权**（设计定位为内网测试工具）：任何能连到 5757 端口的程序都能伪装客户端或收到配置，
  请勿把端口暴露到公网；异常连接（1 MiB 内无换行）会被服务端主动断开
