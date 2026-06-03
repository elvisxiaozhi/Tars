# Step — manager 期望状态对账（修下午停机暴露的自启 bug）

## 概要

修 `tools/manager.py` 一个可靠性 bug：**manager 自身被重启后不会拉起 bot**，导致任何让 manager 重启的事件都造成 bot 停机直到手动干预。改成**持久化"期望状态"（desired state）+ 启动时对账**：bot 本该在跑就自动拉回，你**故意停**的则跨重启也尊重、不拉。**只动 manager.py（进程守护），零触碰交易逻辑、不重编 bot。**

## 背景：2026-06-03 下午停机的根因

`unattended-upgrades`（Ubuntu 自动更新）06:50 UTC 升级共享库（liblzma5/xz-utils/nginx）+ 装新内核 → `needrestart`（自动模式 `-m u`）`systemctl restart polymarket-manager.service` → cgroup 连坐杀死 bot（pid 3635209，**无 core dump=非 SIGSEGV/OOM**）→ **manager 不自动拉 bot → ~20min 空窗**（06:50→07:10）。非崩溃、与 trend_v3/任何代码无关。

读 `_monitor_loop` 确认 bug：它只重启"自己 spawn 过、然后 `poll()!=0` 崩溃"的 bot；manager fresh 启动时 `_bot_proc=None`，监控循环整段跳过 → 永不自启。

## 设计：持久化期望状态 + 启动对账

**核心规则：只有显式 `/api/shutdown` 才写 `stopped`；其它任何消失都视为 `running` → 拉回。**

| bot 消失原因 | desired 判定 | 行为 | 机制 |
|---|---|---|---|
| 手动停（dashboard/`/api/shutdown`）| 落盘 `stopped` | **不拉** | 现有 `_auto_restart=False` + 新落盘 |
| 崩溃（SIGSEGV 等非 0 退出）| 仍 `running` | 拉（5s→300s 退避）| 现有 `_monitor_loop`（manager 活着时）|
| 连坐杀（needrestart/重启 → cgroup 杀 bot+manager）| 仍 `running` | 拉 | **新：startup 读 desired 对账** |
| 直接 kill（手动/外部）| 仍 `running` | 拉 | 现有 monitor（manager 活）或 startup（manager 也死）|

两套机制互补：**bot 崩但 manager 活** → monitor 救；**manager 自己也死**（下午那种）→ 新 manager 启动对账救。

`desired` 默认 `running`（文件缺失时 fail toward availability），只在显式 shutdown 时翻 `stopped`。

## 文件清单（只 1 个，纯 Python 守护，无交易逻辑）

| 操作 | 文件 | 内容 |
|------|------|------|
| 修改 | `tools/manager.py` | (1) `_DESIRED_STATE_FILE` 常量 (2) `_read/_write_desired_state` helper (3) `/api/start` 写 `running` (4) `/api/shutdown` 写 `stopped` (5) `main()` 启动时对账：未在跑且 desired=running 则 `_spawn_bot()` |
| 新增（运行时）| `logs/bot.desired` | 文本 `running`/`stopped`，首次显式 start/stop 时创建 |

**未碰**：`strategy.cpp`、`experiment_engine.cpp`（main/trend_v2/trend_v3）、风控、入场/出场/TP/仓位、config、bot 二进制（仍 `32bc102`，**不重编**）。改变的只是"bot 进程何时被启动"，不改"怎么交易"。

## 设计决策

- **持久化到文件而非内存**：现有 `_auto_restart` 是内存全局，manager 一重启就丢、回默认 `True`——这正是"天真版自启会复活手动停的 bot"的根源。落盘 desired state 才能跨 manager 重启记住意图。
- **默认 `running`（文件缺失）**：对一个"本就常驻"的交易 bot，可用性优先；只有你明确停才记 `stopped`。首次部署即生效（无文件 → 自动拉）。
- **startup 对账只在 `not already_running` 时 spawn**：防止 manager-only 重启时 bot 侥幸存活又被双开。
- **不改 needrestart**（可选项，未做）：本步用 manager 自愈覆盖了 needrestart 的副作用；要不要再把 manager 加进 needrestart 黑名单（让库升级根本不动它）留作后续，非必需。

## 红线遵守

- [x] 没有引入硬编码密钥
- [x] 没有跳过错误处理（desired 读写都 try/except，失败不影响主流程）
- [x] `python3 -m py_compile tools/manager.py` 通过

## 部署计划（等 review 后）

**比改 bot 轻**：dashboard.h 那套不涉及，只动 manager.py（manager 服务运行的 Python）。
1. scp `tools/manager.py`。
2. `systemctl restart polymarket-manager.service`。
3. **自验证**：manager 重启会 cgroup 杀掉当前 bot（3753593），新 manager 启动对账（`bot.desired` 缺失→默认 running）→ **自动拉起新 bot**（日志应见 `startup auto-spawn (desired=running) pid=...`）。无需手动 `/api/start`。
4. 验证 `/api/manager/status` running + `/api/experiment-trend-v3/status` 有响应 + candidates 落盘。
5. 功能验证（可选）：`POST /api/shutdown` → 确认 `logs/bot.desired` 内容变 `stopped` 且 bot 不被 monitor 拉回；再 `POST /api/start` → 变 `running` + 拉起。

## 后续

- **pending reboot 仍在**（新内核 6.8.0-124 已装、未 boot）。本步修复后，将来真重启服务器，bot 会自动回来（desired=running）——可放心择机 reboot 清掉旧内核。
- 可选：manager 加入 needrestart 黑名单，避免库升级时无谓重启 manager（治本于触发源）。
