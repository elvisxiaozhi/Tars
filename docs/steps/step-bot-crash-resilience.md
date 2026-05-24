# Step bot-crash-resilience — bot 崩溃自愈 + 可定位

## 概要

排查 2026-05-24 10:00 UTC 服务器 bot 段错误（SIGSEGV），并落三件套防再发/可定位：
①（运维）立即重启 bot 恢复 dry 采集；② manager 加 bot 崩溃自动重启（带 crash-loop 退避）；
③ 开 core dump + 默认带调试符号构建，让下次崩溃能定位到具体源码行。

## 事故复盘

- **现象**：`polymarket-arb`（pid 1592380，5/23 11:18 起跑约 23h）在 5/24 10:00 SIGSEGV。
  manager（dashboard）一直存活，崩的只是它的 bot 子进程。
- **证据**：
  - `dmesg`：`polymarket-arb[1592380]: segfault at 6439 ... error 4`（用户态读极小地址，野指针/堆损坏）。
  - `addr2line -e build/polymarket-arb 0x10a49c` → `MarketFeed::fetch_from_gamma(vector<CoinStrategyConfig> const&)`。
  - manager 报 `bot_exit_code: -11`（= 信号 11）。
- **为什么停摆数小时**：`manager.py:_monitor_loop` 只把状态标 `crashed`，**无重启逻辑**；
  systemd `Restart=always` 只守护 Python manager，管不到其子进程。
- **为什么定不到行**：服务器 `Release` 构建无 `-g`；`ulimit -c=0` + apport，无 core dump。
- **根因（未钉死）**：`markets_` 实为主线程单线程访问（已排除数据竞争：std::async 只抓 Binance 价格、
  WS 只写 quote_cache、API 只读加锁 state 快照）。故为**确定性内存安全 bug**——更早某处越界写/UAF
  破坏堆，遍历 `markets_` 时踩中。本步先保证"崩了能自愈 + 下次能定位"，根因待 core dump 落地后再追。
- 干扰项排除：5/20 dmesg 的 OOM 杀的是 `cc1plus`（编译期内存爆），与运行崩溃无关；
  manager 的 BrokenPipe 是公网扫描器断连，无害。

## 关键命令

```bash
# ① 止血（已执行）：重启 bot
curl -s -X POST http://127.0.0.1:9090/api/start          # → 新 pid 1763803，status running

# ③ 部署后：开 core dump（服务器，root）
bash tools/enable_coredumps.sh

# ③ 重新配置为带符号构建（服务器；-j2 防 2GB 机编译 OOM）
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j2

# 崩溃后分析 core
gdb build/polymarket-arb /root/polymarket/cores/core.*   # bt full
```

## 文件清单

| 操作 | 文件 |
|------|------|
| 修改 | `tools/manager.py`（`_spawn_bot()` 抽取 + `_monitor_loop` 崩溃自动重启 + 退避；`_start_bot`/`_shutdown_bot` 配合 `_auto_restart` 标志） |
| 修改 | `CMakeLists.txt`（未指定时默认 `RelWithDebInfo`，带 `-g`） |
| 新增 | `tools/enable_coredumps.sh`（部署机开 core dump：core_pattern + LimitCORE drop-in + 运行时 prlimit） |
| 新增 | `docs/steps/step-bot-crash-resilience.md`（本报告） |

## 设计决策

- **自动重启放在 manager 而非改 systemd**：当前架构 manager 拥有 bot 生命周期（dashboard 控制），
  在 `_monitor_loop` 内 respawn 最贴合，不破坏现有拓扑。
- **只在非 0 退出码重启**：clean exit（code 0，主动 shutdown）不重启；`_auto_restart` 标志再防
  shutdown 竞态导致误重启。
- **crash-loop 退避**：启动后 <60s 又崩则指数退避（5s→…→封顶 300s），避免崩溃循环刷屏/打满日志；
  健康跑过 60s 后再崩则视为首次（5s 即重启）。
- **core dump 用 prlimit 即时生效**：rlimit 在 fork 时继承，给运行中的 manager `prlimit` 放开后，
  其之后启动的 bot 即带 unlimited core，**无需重启 manager**（避免误杀 cgroup 内 bot）；
  同时写 systemd drop-in 保证 manager 下次重启仍持久。
- **RelWithDebInfo 而非 Debug**：保留 `-O2`（性能等同 Release），仅加 `-g`，对实时策略无影响。

## 红线遵守

- [x] 没有引入硬编码密钥
- [x] 没有跳过错误处理（`_spawn_bot` try/except；自动重启失败有日志）
- [x] manager.py `py_compile` 通过；`enable_coredumps.sh` `bash -n` 通过；CMake configure 通过（Build type = RelWithDebInfo）
- [x] 未触碰交易/风控/签名逻辑，纯生命周期与构建

## 验收结果

```
$ python3 -m py_compile tools/manager.py   → OK
$ bash -n tools/enable_coredumps.sh        → OK
$ cmake -S . -B /tmp/pmtest                → -- Build type = RelWithDebInfo  (exit 0)
$ curl -s .../api/manager/status (重启后)  → {"bot_status":"running","bot_pid":1763803,"bot_exit_code":null}
```

## 遗留问题 / 部署须知（等 review 后做）

1. **未提交、未部署**：本步仅本地改动，按工作流等 review。`tools/manager.py` 另含一处早先未提交的
   dashboard 热加载改动（L144），一并 review。
2. **部署 manager.py**：服务器无 git，需 scp 覆盖 `/root/polymarket/tools/manager.py` 后
   `systemctl restart polymarket-manager.service`。**注意**：重启 manager 会随 cgroup 杀掉当前 bot
   （pid 1763803），重启后需再 `POST /api/start`（manager 不会开机自动拉 bot——本步未加该行为）。
3. **带符号重编**：2GB 机加 `-g` 编译内存更紧（5/20 已有 cc1plus OOM 史），用 `-j2`、依赖现有 2G swap；
   或在本地/大机编译后 scp 二进制。
4. **根因仍未定位**：core dump + 带符号构建就绪后，等下一次复现拿 `bt full` 再追真凶
   （疑似越界写/UAF 破坏 `markets_`）。可选：本地用历史数据跑 ASan 构建主动揪。
```
