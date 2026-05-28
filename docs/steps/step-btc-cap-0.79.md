# Step — BTC momentum 入场上沿 coin-aware 收到 0.79

## 概要

把 momentum_follow 入场上沿从全币种统一 0.84 改成 coin-aware：**BTC = 0.79，其余币种维持 0.84**。砍掉 BTC 唯一负 EV 子带 0.80-0.84。延续 [[momentum-band-postmortem]] 的「BTC→XRP→DOGE→ETH 逐个收口」计划第一步。

## 背景：0.84 扩宽 48h 验收失败

commit 5a33bad（2026-05-26）把上沿 0.67→0.84，基于回测预期 90% 胜 / EV +$0.084。48h 实盘 dry-run 验收【失败】：扩宽新增带 52% 胜 / EV −$0.003。失败集中在 DOGE 与深价 0.80-0.84。

## 本步数据复核（2026-05-29，服务器 trades.jsonl，仓位级口径）

复核方法：按 `(entry_time, coin)` 去重成仓位（`id`/`armed_at_ms` 都不可靠，见遗留问题），每仓 pnl = 同 key 全 tranche 之和，`regime=="trend"` 即 momentum_follow，按 `entry_price` 分子带。

BTC trend 全部仓位（42 仓）：

| 子带 | n | 胜率 | EV/笔 | 净 |
|---|---|---|---|---|
| ≤0.67 | 11 | 73% | +0.083 | +0.909 |
| 0.67-0.74 | 9 | 78% | +0.056 | +0.501 |
| 0.74-0.79 | 6 | 67% | +0.063 | +0.379 |
| **0.79-0.84** | 16 | **56%** | **−0.011** | **−0.179** ← 唯一负 EV 带 |

封顶对比：

| 方案 | n | 胜率 | EV/笔 | 净 |
|---|---|---|---|---|
| cap 0.84（当前） | 42 | 67% | +0.038 | +1.610 |
| **cap 0.79（本步）** | 26 | **73%** | **+0.069** | **+1.789** |

砍掉 0.80-0.84 后 EV/笔 翻 ~1.8 倍、胜率 +6pp、净反而上升（被砍带本身负贡献）。该结论与 postmortem 表格逐行吻合（独立复现）。

## 关键命令

```bash
cmake --build build -j           # 编译
scp root@159.203.56.165:/root/polymarket/logs/trades.jsonl .cache/   # 拉数据
python3 .cache/btc_band_recheck.py   # 复核脚本（仓位级分子带）
```

## 文件清单

| 操作 | 文件 |
|------|------|
| 修改 | `src/core/strategy.cpp`（`evaluate_momentum_follow` 上沿改 coin-aware） |

## 设计决策

- **coin-aware 而非全币种统一**：postmortem 证明失败按币种/深度不均匀，BTC 0.64-0.79 实打实 +$0.88，不该按 ADR S7 字面全退回 0.67。先只动 BTC，其余维持 0.84 待逐个复核。
- **上沿取 0.79 而非更保守的 0.74**：0.74-0.79 子带仍 67% 胜 / +$0.063 EV（正 EV，过 +$0.02 门槛），扔掉它少挣 ~$0.38。判盈利用 **EV/笔 ≥ +$0.02**，不用 ADR S7 那个被证伪回测抄来的 75% 胜率门槛。
- **比较用 `entry_price > entry_upper + 1e-9`**：保留边界 0.79/0.84 entry（entry 落在 0.01 网格上，1e-9 容差防浮点噪声误杀）。
- **不动 experiment_engine**：trend_v2 等 shadow 实验有独立 band（0.64-0.67），不在本步范围。

## 红线遵守

- [x] 没有引入硬编码密钥
- [x] 没有跳过错误处理
- [x] 编译通过，无 warning

## 验收结果

```
[ 61%] Building CXX object CMakeFiles/polymarket-arb.dir/src/core/strategy.cpp.o
[ 63%] Linking CXX executable polymarket-arb
[100%] Built target polymarket-arb
```

## 遗留问题

1. **bot 运行正常（此前误判已纠正）**：bot 实际由 `polymarket-manager.service`（active，已起 2 天）拉起，其子进程 `polymarket-arb` 实时跑（heartbeat #10982、trades.jsonl 持续增长）。`polymarket.service` 是**已 disabled 的旧 unit**，别再用它判活。此前「bot DOWN」是误判：查错了 unit + 拿本地 05-29 对比服务器（NTP 已同步、当前 05-28）的日志时间戳。部署仍未做，等 review。
2. **`code_version` 字段失真**：运行二进制（build 时间 05-26 14:33，含 0.84 逻辑）给成交打的却是旧 hash `e0e9e26`（0.67-cap 的 commit）。code_version 不能当部署标记用，应改成 build 时注入真实 git hash。
3. **`armed_at_ms` 不可靠**：大量仓位该字段为 0/缺失（恰好命中负 EV 仓位），用它做时间过滤会系统性筛掉亏损样本、造出假 100% 胜率。仓位级聚合/过滤一律用 `entry_time`。
4. **逐币收口待续**：XRP（边际 +$0.007）/ DOGE（拟剔除 −$1.51）/ ETH（打平）尚未单独动手。
