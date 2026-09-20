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

### 3.1 若把整个 `diskstress` 目录作为仓库

用仓库根目录的 `.github/workflows/build-start.yml`；本目录下的 `start/.github/workflows/build.yml` 可以删掉。

### 3.2 若只把 `start` 目录作为仓库根目录

```bash
cd start
git init
git add .
git commit -m "DiskStress v1.1.0"
git branch -M main
git remote add origin https://github.com/<账号>/<仓库>.git
git push -u origin main
```

仓库页面 → **Actions** → `Build DiskStressStart` → **Run workflow** 可手动触发。
产物在 job 底部 **Artifacts** 下载：`DiskStressStart-x64` / `-x86`。

打 tag 自动挂 Release：

```bash
git tag v1.1.0 && git push origin v1.1.0
```

本机也能编译：`build.bat`（x64 Native Tools Command Prompt）。

---

## 4. 部署到服务器

```bat
:: 以管理员身份执行
DiskStressStart.exe --install
sc query DiskStressService          :: 确认 STATE: RUNNING
```

报告目录：`C:\ProgramData\DiskStress\reports\`
运行日志：`C:\ProgramData\DiskStress\stop.log`
卸载：`DiskStressStart.exe --uninstall`

> `DiskStress.ini` 必须和 exe 放**同一个目录**，服务启动时从 exe 所在目录读取。

---

## 5. 磁盘占用策略

- **压力文件在系统 TMP 目录**：`GetTempPath()` → `…\AppData\Local\Temp\DiskStress\stress.dat`（服务模式下是 `C:\Windows\Temp\…`），可在 ini 改 `FilePath`。
- **逻辑工作集 `WorkingSetGiB=10`**：文件逻辑长度 10 GiB，随机偏移覆盖该范围，保证 LBA 跨度大于 SSD 缓存。
- **封顶即重建 `PhysicalCapGiB=10`**：每 512 次写入采样真实占用（不是文件大小），达到 10 GiB 就**删除整个文件并重新生成**，占用曲线 0 → 10 GiB → 0 …。
- **退出即删 `DeleteFileOnExit=1`**。

---

## 6. 配置（`DiskStress.ini`）

| 键 | 默认值 | 说明 |
|---|---|---|
| `FilePath` | 系统 TMP 目录 `DiskStress\stress.dat` | 压力文件（临时文件，会被删除） |
| `WorkingSetGiB` | `10` | 逻辑工作集（文件长度），应大于 SSD 缓存 |
| `PhysicalCapGiB` | `10` | 真实占用封顶，达到后删除重建；`0` = 不封顶 |
| `DeleteFileOnExit` | `1` | 停止时删除压力文件 |
| `BlockBytes` | `4096` | 单次写入大小，需为扇区整数倍 |
| `NoBuffering` | `1` | 绕过系统缓存；`0` = 使用系统缓存 |
| `ReportIntervalMinutes` | `300` | 报告周期（5 小时） |
| `SegmentMinutes` | `10` | 报告内分段窗口 |
| `ReportDir` | `C:\ProgramData\DiskStress\reports` | 报告目录，最多保留 3 份 |
| `StateDir` | `C:\ProgramData\DiskStress` | `worker.pid` / `stop.log` |

改配置后需**停止再启动**服务才生效。

---

## 7. 报告内容

每 5 小时一份，目录内最多保留 3 份，UTF-8 文本：

1. 基本信息（起止时间、运行时长、产生原因、PID）
2. 测试配置（工作集、封顶、块大小、缓存策略）
3. 系统与环境（OS、CPU、内存、卷容量、文件系统、扇区）
4. 写入统计总览（总次数/字节、真实占用采样、封顶重建次数、IOPS、吞吐、写/sync 延迟）
5. fsync 延迟分位数 P50/P90/P95/P99/P99.9 + ASCII 直方图
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
