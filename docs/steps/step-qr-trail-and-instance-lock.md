# Step — QR breakeven-trail + 单实例 flock

## 概要

修两件事：
1. **P1（必修 BUG）**：bot 启动时拿 `logs/polymarket-arb.pid` 上 `flock(LOCK_EX|LOCK_NB)`，已有实例则立即退出。根因是 `systemd polymarket.service` 和 `tools/manager.py` 各自能 spawn 一个 bot，两个进程的 `next_position_id` 都从 1 起算并写同一份 `trades.jsonl`，2026-05-21 14:16 实证导致同条 BTC 被双开（同 `P1` 双 entry、亏损翻倍）。
2. **P0（策略优化）**：主策略 QR 分支加 breakeven-trail——`max_price ≥ entry+0.05` 后若 `current ≤ entry+0.02` 立即退出（`exit_reason=qr_breakeven_trail`）。

## 关键命令

```bash
# 编译
cmake --build build -j

# 跑 KAT
./build/test_crypto && ./build/test_chain && ./build/test_eip712

# 部署后验证 flock：故意启第二个实例应失败
./build/polymarket-arb ./config/config.json  # 第二个会 spdlog::error 并 return 1
```

## 文件清单

| 操作 | 文件 |
|------|------|
| 修改 | `src/main.cpp` —— include `<fcntl.h>/<sys/file.h>/<unistd.h>`；`init_logging` 之后加 flock 块（约 35 行） |
| 修改 | `src/core/strategy.cpp` —— `evaluate_exit` 的 `QUIET_REVERSION` 分支顶部加 breakeven-trail（13 行 + 注释） |

## 设计决策

### P1 锁选 flock 而不是 lockfile / dbus / mkdir

- **flock(LOCK_EX|LOCK_NB)**：内核级，进程死了 OS 自动释放。
- **mkdir/lockfile** 在 crash 后会残留导致下次启动失败，要写 stale-lock 检测，复杂且易错。
- **dbus / systemd unit Restart=** 治标不治本：manager.py 还能绕开。
- pid 文件放 `logs/`（cfg.logging.file 父目录），不污染源码目录。文件内容写 pid 仅供 `spdlog::error` 排查显示，**不依赖它做判断**（判断只看 flock 结果）。

### P1 fd 终生持有

故意把 `lock_fd` 留给一个函数 scope-static `s_lock_fd` 引用，避免任何 close 路径。进程退出时 OS 自动释放 flock；exec 系列调用不会继承 flock（即便 systemd 触发 restart 也正确——新进程重新争锁）。

### P0 触发参数 0.05 / +0.02

- `mfe ≥ 0.05`：等价于"价格曾经走出 +5 个点"，对应历史 12 笔 QR 中 6 笔 max_price 在 0.30 以上。再小（如 0.03）容易把假信号也触发；再大（如 0.08）很多损单根本到不了。
- `current ≤ entry+0.02`：保留 +0.02 利润作为退出价（覆盖手续费），不至于退在 entry 上方很多。
- **保护大赢家**：两笔历史大赢家（ETH 0.21→0.46→0.39 和 BTC 0.25→0.77→0.66）的 exit_price 都远高于 entry+0.02，规则对它们 no-op。
- **不动 TP 体系**：TP0=0.42 / TP1=0.62 保留——breakeven-trail 只在价格没能站稳 +0.05 时兜底。

### P0 反事实模拟（12 笔历史 QR 数据）

`-$0.70 → +$1.07`，主要贡献来自 5 笔从亏 -$0.24~-$0.36 转为 +$0.05 的损单转化。模拟用的是 `max_price` 而不是 mfe_at_5min，准确还原了 trail 是实时触发的。代码也是按 `pos.max_price`（实时更新）判断，与模拟一致。

## 红线遵守

- [x] 没有引入硬编码密钥
- [x] 没有跳过错误处理（flock 失败 / open 失败都 spdlog::error + return 1）
- [x] 编译通过，无新增 warning
- [x] 没有改 V2 字段 / 三层账户 / 代理 / 单仓制
- [x] 不写 live 路径（QR 改动只在 `evaluate_exit`，对 dry / live 一视同仁；flock 也是）

## 验收结果

```
$ cmake --build build -j
[100%] Built target polymarket-arb

$ ./build/test_crypto && ./build/test_chain && ./build/test_eip712
===== 40 passed, 0 failed =====
=== ALL PASS ===
```

QR trail 触发的实时验证需要等下次 0.25-0.26 cheap_value 入场（每日 1-2 笔级别），届时日志会出现 `QR BREAKEVEN TRAIL: ...` warn 行，jsonl `exit_reason=qr_breakeven_trail`。

flock 验证可在服务器 deploy 后手动启第二个 `polymarket-arb` 二进制：第二个应立刻 `spdlog::error` + `return 1`。systemd unit 不需要任何改动。

## 遗留问题

1. **实验引擎（ExperimentEngine）没改**——eth_cheap_v1 / eth_only_v1 这些也是 cheap_value 风格，本步只动了主策略 `evaluate_exit`。下一步再 mirror 一份 trail 到 `evaluate_eth_only_exit` 等位置（待主策略 trail 累计 ≥10 笔数据后再决定是否扩散）。
2. **manager.py 的 `/api/start`** 路由仍可被外部 POST 触发，被 flock 挡住后会写 systemd journal 的 `Refusing to start` 错误日志——manager 不会感知到失败，因为它只关心 popen 返回。需要 manager.py 也读 polymarket-arb.pid 做更友好的报错（后续）。
3. **历史 trades.jsonl 已被污染**——双进程时期写入的 ~10 条记录无法精确剥离。`tools/` 下加一个去重工具按 entry_time 精确匹配剔除是个 follow-up，但量小不阻塞分析。
4. **trend_v2 stop_btc 偏紧** + **DOGE 是否从主策略 trend 桶剔除**：P2/P3，留待样本扩大。
