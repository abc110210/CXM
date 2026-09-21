# DiskStressStart — 静默后台压力测试（服务端 / 开机自启）

4 KiB 随机写入 + 每次 fsync 的磁盘压力测试。**无任何界面、不进任务栏**，可注册为 **Windows 服务开机自启**，装上后就一直跑，直到用「DiskStress 控制台」停止。

> 本目录在 GitHub 上编译（`.github/workflows/build.yml`），产出 `DiskStressStart.exe`。
> 带界面的停止器在 `../stop/`，由你自己本地编译。

---

## 1. 三种运行方式

| 方式 | 命令 / 操作 | 说明 |
|---|---|---|
| 注册为服务（**推荐，服务器用**） | `DiskStressStart.exe --install`（管理员） | 创建 `DiskStressService`，启动类型 **自动**，装完立即启动；开机无需登录即运行 |
| 卸载服务 | `DiskStressStart.exe --uninstall`（管理员） | 先停止再删除服务 |
| 手动静默运行 | 双击 `DiskStressStart.exe`（无参数） | 当前用户会话内后台跑，无任何窗口；单实例 |

服务模式下 SCM 调用 `--service` 参数启动，不要手动加这个参数。

服务额外配置了**失败自动重启**（10 s / 30 s 后各重启一次），保证长期运行不掉线。

---

## 2. 运行约束

- **一直运行**：装成服务后除非被停止，否则无限运行；每 5 小时产出一份报告。
- **停止时机**：只能通过停止器（或服务停止指令）触发；停止瞬间会**先生成最终报告**再退出，最长等待约 3 分钟。
- **防休眠**：运行期间阻止系统进入睡眠。
- **停止后清理**：删除压力文件，磁盘占用归零。

---

## 3. 在 GitHub 上编译

编译脚本与本工程一一对应，共两份（各自的 workflow + 各自的 bat）：

| 位置 | 文件 | 用途 |
|---|---|---|
| 仓库根 | `.github/workflows/build-start.yml` | GitHub Actions 编译 `start/`（只在 `start/**` 变更时触发） |
| 本目录 | `build_start.bat` | 本机 MSVC 编译（x64 Native Tools Command Prompt 里执行） |

### 3.1 整个 `diskstress` 目录作为一个仓库（推荐）

```bash
git init
git add .
git commit -m "DiskStress v1.2.0"
git branch -M main
git remote add origin https://github.com/<账号>/<仓库>.git
git push -u origin main
```

仓库页面 → **Actions** → `Build DiskStressStart` → **Run workflow** 可手动触发。
产物在 job 底部 **Artifacts**：`DiskStressStart`（x64 单版本）。

打 tag 自动挂 Release：

```bash
git tag v1.2.0 && git push origin v1.2.0
```

### 3.2 只把 `start` 目录作为仓库根目录

把根目录的 `build-start.yml` 复制到 `start/.github/workflows/build.yml`，并把命令里的路径前缀 `start\src\` 改成 `src\` 即可。

本机编译：`build_start.bat` → 产出 `out\DiskStressStart.exe`。

---

## 4. 部署到服务器

```bat
:: 以管理员身份执行
DiskStressStart.exe --install
sc query DiskStressService          :: 确认 STATE: RUNNING
```

报告目录：`C:\ProgramData\DiskStress\reports\`
运行日志：`C:\ProgramData\DiskStress\stop.log`
调试日志：`C:\ProgramData\DiskStress\debug.log`
卸载：`DiskStressStart.exe --uninstall`

> 无需任何配置文件，参数已写死在代码里。

### 换目录 / 换机器

程序**不依赖 exe 所在目录**：日志、报告、压力文件路径都是固定的（ProgramData + 系统 TMP），搬走 exe 不影响任何行为。

安装命令是**覆盖式**的：把 `DiskStressStart.exe` 和 bat 一起拷到新目录，管理员运行一次 bat，它会——

1. 发现同名服务已存在 → 先停止旧实例
2. **把服务指向新的 exe 路径**（`ChangeServiceConfigW`）
3. 重设延迟自启、失败重启策略，再启动

所以搬家后**不需要先 `--uninstall`**，跑一次 bat 就完成迁移。验证看 bat 输出的 `sc qc` 里的 `BINARY_PATH_NAME` 是否已是新路径。

---

## 3.3 运行依赖

编译参数加了 **`/MT`（静态链接 CRT）**，生成的 exe **不依赖 VC++ 运行库**，拷到任何 Win10/Server 机器上都能直接跑。
（若你用的是更早的产物，可能在没装 Microsoft Visual C++ Redistributable 2015-2022 x64 的机器上**静默启动失败**：无窗口、无日志，看起来跟没运行一样。）

## 3.4 跑不起来怎么排查

按顺序查：

1. **看进程**：任务管理器 → 详细信息 → 有没有 `DiskStressStart.exe`（服务模式下进程名同样是它）。
2. **看日志**：`C:\ProgramData\DiskStress\debug.log` —— 程序启动的第一行日志在任何判断之前就写入。
   - 若这里都没有 → 进程没加载起来（缺运行库 / 架构不符 / 被杀软拦截）。
   - 日志目录不可写时会自动兜底到 **exe 同目录 `\DiskStress\`**，再不行到 **系统 TMP `\DiskStress\`**，实际路径会记在 `debug.log` 的"日志目录"一行。
3. **看系统事件**：事件查看器 → Windows 日志 → 应用程序，找 `SideBySide` 或 `应用程序错误` 条目。
4. **服务是否注册**：`sc query DiskStressService`，状态应为 `RUNNING`；用 `--install` 注册时需要管理员命令行。
5. **双击无效但服务正常**：这是正常的，交互模式一旦被单实例拦截也会写日志，看 `单实例检查` 那行。

## 4.1 防止多开

启动时创建**跨会话全局互斥体** `Global\DiskStress_SingleInstance_v1`（DACL 放行 Everyone 的同步权限），服务模式与双击模式共用同一把锁：

- 服务已在跑 → 再双击 exe 会直接退出，不会起第二个写入进程；
- 双击已跑 → 再安装/启动服务时服务启动中止并上报 `ERROR_SERVICE_ALREADY_RUNNING`；
- 创建 `Global\` 失败（权限不足）时自动退回会话内同名互斥体。

判定结果会写进 `debug.log`（`单实例检查` 一行）。

## 4.2 调试日志 `debug.log`

路径：`C:\ProgramData\DiskStress\debug.log`（UTF-8，带毫秒时间戳，追加写入）。
启动是否成功，看这个文件即可，每一步都有 `[OK]` / `[FAIL]` 自检：

| 检查项 | 说明 |
|---|---|
| `进程启动` / `配置加载` | 参数、PID、压力文件路径、覆盖区大小、报告目录 |
| `单实例检查` | 是否已有实例在运行（FAIL = 被多开拦截） |
| `服务入口` / `服务模式` | SCM 控制处理器注册、控制分发器连接 |
| `状态目录` / `报告目录` | 目录创建是否成功 |
| `PID 文件` | worker.pid 写入结果 |
| `防休眠` | `SetThreadExecutionState` 是否生效 |
| `可用空间` | 卷剩余空间、覆盖区是否被下调 |
| `扇区大小` | 每扇区字节数 → 决定 NO_BUFFERING 还是回退缓存模式 |
| `打开压力文件` / `回退打开` | CreateFile 结果，失败会带 err= 错误码 |
| `设置文件长度` / `文件实际长度` | 是否成功设为 2 GiB |
| `分配写缓冲` | 4 KiB 对齐缓冲区分配 |
| `停止事件` | Global / 会话内两个事件创建情况 |
| `进入写入循环` | 块数、缓存模式、报告周期 |
| **`首次写入`** | 最关键一行：WriteFile 与 FlushFileBuffers 是否都成功、实际字节数、首次 fsync 耗时 |
| `心跳` | 每 10000 次写入一行：累计次数/字节、实际占用、写错误、同步错误 |
| `周期报告` / `最终报告` | 报告文件路径，失败会标 FAIL |
| `停止原因` / `删除压力文件` / `退出` | 收尾每一步的结果与返回码 |

排查顺序：先看有没有 `进入写入循环`，再看 `首次写入` 是不是 `[OK]`；`[FAIL]` 行都带 `err=` 错误码。

## 5. 写入模式与磁盘占用

### 5.1 高队列异步 I/O

- **小块 I/O**：默认 **4 KiB** 随机偏移，块大小可配置（512 B / 4 K / 8 K / 16 K / 32 K / 64 K）。
- **反复覆盖已有数据**：启动时用 1 MiB 顺序写把 2 GiB 覆盖区**预填充**一遍（debug.log 里有耗时与吞吐），之后所有随机写都命中已分配块，是纯覆盖、不再分配新簇——最接近 SSD 稳态表现。
- **高 IOPS / 高队列深度**：`FILE_FLAG_OVERLAPPED` 异步 I/O，**4 线程 × QD8 = 最多 32 个未完成 I/O** 同时在飞。
- **fsync 策略**：改为**周期性** `FlushFileBuffers`（默认 10 s 一次，独立线程执行），退出前再强制 fsync 一次。**不再每次写都 fsync** —— 这是拿到高 IOPS 的前提（每次 fsync 会把队列深度压回 1）。
- **延迟口径**：报告的「完成延迟」= 提交到完成的设备延迟；「周期 fsync」单独统计次数、平均与最大。

### 5.2 磁盘占用

- **压力文件在系统 TMP 目录**：`GetTempPath()` → `…\AppData\Local\Temp\DiskStress\stress.dat`（服务模式下是 `C:\Windows\Temp\DiskStress\stress.dat`）。
- **长度固定 2 GiB**：启动时设为 2 GiB 并预填充，随机偏移只在这个区间内反复覆盖，文件不再增长。
- **占用上限 = 2 GiB**：真实占用（"占用空间"）恒等于覆盖区大小，不会超过。
- **退出即删**：停止时删除压力文件，占用归零；若上次被强杀留下残留，下次启动会先清理。

---

## 6. 配置（写死在代码里，无 ini）

所有参数集中在 `src/shared.h` 顶部的常量区，改完重新编译：

| 常量 | 默认值 | 说明 |
|---|---|---|
| `kWorkingSetBytes` | `2 GiB` | 固定覆盖区大小，同时就是占用上限 |
| `kBlockBytes` | `4096` | 单次写入块大小，需为扇区整数倍；可改 512 / 8192 / 16384 / 32768 / 65536 |
| `kThreads` | `4` | 并发写入线程数 |
| `kQueueDepth` | `8` | 每线程未完成 I/O 数（总队列深度 = 线程 x QD = 32） |
| `kSyncIntervalSec` | `10` | 周期性 `FlushFileBuffers` 间隔（不是每次写都 fsync） |
| `kPrefill` | `true` | 启动时写满整个覆盖区，之后每次写都是覆盖已有数据 |
| `kReportIntervalSec` | `18000`（5 小时） | 报告周期 |
| `kSegmentSec` | `600`（10 分钟） | 报告内分段窗口 |
| `kMaxReports` | `3` | 报告目录内最多保留份数 |
| `kNoBuffering` | `true` | 绕过系统缓存；`false` = 使用系统缓存 |
| `kDeleteOnExit` | `true` | 停止时删除压力文件 |
| `kStateDir` | `C://ProgramData//DiskStress` | `worker.pid` / `stop.log` |
| `kReportDir` | `C://ProgramData//DiskStress//reports` | 报告目录 |
| `kStressFileSubDir` / `kStressFileName` | `DiskStress` / `stress.dat` | 位于系统 TMP 目录下 |

改动后需**重新编译并重启服务**才生效。

---

## 7. 报告内容

每 5 小时一份，目录内最多保留 3 份，UTF-8 文本：

1. 基本信息（起止时间、运行时长、产生原因、PID）
2. 测试配置（工作集、封顶、块大小、缓存策略）
3. 系统与环境（OS、CPU、内存、卷容量、文件系统、扇区）
4. 写入统计总览（总次数/字节、真实占用采样、IOPS、吞吐、完成延迟、周期 fsync 次数与延迟）
5. I/O 完成延迟分位数 P50/P90/P95/P99/P99.9 + ASCII 直方图（fsync 延迟单独列出）
6. 每 10 分钟分段统计表
7. 错误与异常
8. 说明

---

## 8. 源码结构

```
start/src/
├─ shared.h/.cpp   配置加载、路径、格式化、QPC 计时、真实占用查询
├─ report.h/.cpp   延迟直方图/分位数、系统信息、报告生成、报告轮转（保留 3 份）
├─ worker.h/.cpp   压力循环（写入 + fsync + 封顶重建 + 报告调度）
└─ main.cpp        --install / --uninstall / --service / 静默直接运行
```

## 9. 注意事项

- 服务以 **LocalSystem** 运行，写 `C:\ProgramData` 与 `C:\Windows\Temp` 无权限问题。
- 停止事件同时创建 `Global\` 与会话内两个命名对象，非管理员的控制台也能发停止信号（服务不存在时的降级路径）。
- 压力文件是消耗品，**不要**指向任何有用文件。
- 非 NTFS 卷（exFAT 等）上真实占用可能超过 100% 覆盖预期，建议在 NTFS 上运行。

---

## 10. 杀毒软件误报与白名单

本工具的行为特征（无窗口后台常驻、服务自启、绕过缓存随机写满磁盘、TCP 上报）会命中杀软启发式的
"勒索软件 / 后门"画像，属**预期内误报**，不是感染。构建已内置 VERSIONINFO 版本信息降低误报率；
如仍被拦截，按需选用：

**① Defender 排除**（管理员 PowerShell，二选一或都加）：

```powershell
Add-MpPreference -ExclusionPath    "E:\zaxiang"
Add-MpPreference -ExclusionProcess "DiskStressStart.exe"
```

**② 解除 SmartScreen 锁定**（GitHub 下载的 exe 自带 Zone 标记）：

```powershell
Unblock-File "E:\zaxiang\DiskStressStart.exe"
```

或右键文件 → 属性 → 勾选"解除锁定" → 确定。

**③ 已被隔离时**：Windows 安全中心 → 病毒和威胁防护 → 保护历史记录 → 还原并允许。

根治误报只有**代码签名证书**一条路（OV 起步约每年几百元，EV 可累积 SmartScreen 信誉）；
内网 / 自有服务器场景用上面三步即可。

### 10.1 关键坑：防篡改保护（Tamper Protection）

Windows 10/11 默认开启"防篡改保护"，**命令行方式添加 Defender 排除会被静默拒绝**——
跑了命令没效果基本都是这个原因。排除项必须走 UI：

> Windows 安全中心 → 病毒和威胁防护 → "管理设置" → 排除项 → 添加排除 → **文件夹** → 选 exe 所在目录

验证排除是否真的生效（管理员 PowerShell）：

```powershell
Get-MpPreference | Select-Object -ExpandProperty ExclusionPath
```

### 10.2 免费根治：自签名 + 本机信任（机器所有者方案）

**全程 0 元、全部用 Windows 内置 PowerShell cmdlet，无需安装任何东西。**
自签名 = 自己给自己发证书，不经过 CA，所以免费；花钱的 OV/EV 证书只在把软件分发给
别人时才需要。管理员 PowerShell 依次执行：

```powershell
# 1) 创建自签代码签名证书（有效期 3 年）
$cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=DiskStress" `
        -CertStoreLocation Cert:\CurrentUser\My -NotAfter (Get-Date).AddYears(3)

# 2) 导出为 pfx（密码自定）
$pwd = ConvertTo-SecureString -String "your-password" -Force -AsPlainText
Export-PfxCertificate -Cert $cert -FilePath .\DiskStress.pfx -Password $pwd

# 3) 装入本机受信任的发布者 + 受信任的根
Import-PfxCertificate -FilePath .\DiskStress.pfx -CertStoreLocation Cert:\LocalMachine\TrustedPublisher -Password $pwd
Import-PfxCertificate -FilePath .\DiskStress.pfx -CertStoreLocation Cert:\LocalMachine\Root -Password $pwd

# 4) 签名（Set-AuthenticodeSignature 为内置 cmdlet，不需要 signtool）
$cert = Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert |
        Where-Object { $_.Subject -like "*DiskStress*" }
Set-AuthenticodeSignature -FilePath .\DiskStressStart.exe -Certificate $cert `
        -TimeStampServer "http://timestamp.digicert.com"

# 5) 验证签名状态应为 Valid
Get-AuthenticodeSignature .\DiskStressStart.exe | Select-Object Status, SignerCertificate
```

说明：
- 时间戳服务器是免费的公共服务，加上后**证书到期签名依然有效**。
- 本机信任只对这一台机器有效；Defender 的行为启发式仍可能拦截，10.1 的排除项仍建议保留。
- **每次重新编译出的 exe 都要重签一次**（第 4 步，两条命令，可写进构建脚本末尾自动化）。
- 要分发给**别人的**机器，才需要购买 OV/EV 证书。

---

## 11. 进程加固（防误杀）

服务的"防杀"由 SCM 失败自动重启实现（无需看门狗进程，不增加杀软误报）：

| 场景 | SCM 判定 | 结果 |
|---|---|---|
| `taskkill /f` 强杀、崩溃、断电外的异常退出 | 服务**失败** | 按重启链自动复活：**5 s → 10 s → 30 s → 永远每 30 s**（`dwResetPeriod=0` 计数不重置） |
| `DiskStress_Stop.bat` / 停止器 GUI 优雅停止 | 正常退出 | **不会复活**（保住"必须软件关闭"的设计） |
| 服务被 `sc config` 禁用 / failure actions 被清 | —— | 管理员级对抗不做（需内核驱动或 PPL，会引爆杀软误报） |

验证：`sc qfailure DiskStressService` 应显示三段 RESTART (5000/10000/30000 毫秒)，RESET_PERIOD 0。


---

## 12. 看门狗与签名脚本

### 12.1 三方保活环（v1.2 起）

同一个 exe 承担三种角色：`--service`（服务）/ `--watchdog`（看门狗）/ 默认（静默直跑）。

```
服务进程 ── 每 5 分钟检查看门狗互斥体，死了就重新拉起 ──► 看门狗
看门狗   ── 每 60 秒检查服务状态，非 RUNNING 就启动 ──► 服务
```

- 看门狗以 LocalSystem 运行（服务拉起），普通用户无法结束；同名互斥体防多开
- **强杀服务**（taskkill /f）：SCM 失败重启 5 s 内拉起 + 看门狗 60 s 内兜底，双保险
- **正常停止**：任何一条优雅路径（Stop.bat / 停止器 GUI / worker 收到停止）退出前
  都会先置位看门狗停止事件——**看门狗先死，服务再停**，不会被拉活
- 停止全流程（杀看门狗 → 服务写报告删文件 → 退出）总时限 5 分钟以内

### 12.2 签名脚本（根目录，免费）

`DiskStress_Sign.bat`（管理员运行，内含 `DiskStress_Sign.ps1`）：
幂等——证书（CN=DiskStress，3 年）不存在则创建，已存在则复用；
导入本机"受信任的发布者/根"后给同目录**所有 exe** 签名（含时间戳，免费公共服务）。

**流程：每次从 GitHub 下载新 exe → 右键管理员跑一次 DiskStress_Sign.bat → 再跑 DiskStress_Start.bat。**
效果：本机不再弹 SmartScreen 未知发布者；配合排除目录，杀软不再误杀（含看门狗）。
