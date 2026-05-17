# 实验策略（Experiment Strategies）

> 最后更新：2026-05-17（branch `experimental-simulator`）。本文档描述**与主策略并行运行的 5 个 shadow 实验策略**，它们只跑模拟、不下真单，用于评估"如果换成 X 策略会怎样"。
>
> 主策略文档见 [`strategy-btc-1h.md`](./strategy-btc-1h.md)。本文档**不**重复主策略。
>
> 分析实验日志请用 [`../prompts/analyze_strategy.md`](../prompts/analyze_strategy.md)（v2 标准化 prompt）。

---

## 一、实验框架做什么

每个 `ExperimentEngine` 是一份独立的"影子策略":

- **共享主策略的 tick 数据**（同一份 `BtcMarketData` + `quotes`），保证可比性
- **独立 entry/exit/TP 规则**（每个 engine 用自己的 `strategy_name` 路由到 `evaluate_*` 函数）
- **独立持仓 + 独立 $20 起始余额**（`cfg.experiment.initial_balance`）
- **独立 jsonl 落盘**（`./logs/experiment_<name>_trades.jsonl`）
- **永不下真单**——live 模式下也只跑 BTC，且写自己的 jsonl，不动真钱
- **由 `cfg.experiment.enabled` 总开关控制**

实例化位置：`src/main.cpp:347-356`（构造）+ `src/main.cpp:1441/1442/1250/1252/1329` (分发 on_market)。

---

## 二、当前活跃的 5 个实验策略

### 总表（按 main.cpp 实例化顺序）

| 变量名 | strategy_name | id 前缀 | 日志文件 | 市场类型 | 评测代码 |
|---|---|---|---|---|---|
| `experiment` | `eth_cheap_v1` | `C` | `experiment_eth_cheap_v1_trades.jsonl` | Crypto 1h（仅 ETH） | `evaluate_eth_cheap_v1` (L612) |
| `trend_experiment` | `eth_late_cheap_v1` | `L` | `experiment_eth_late_cheap_v1_trades.jsonl` | Crypto 1h（仅 ETH） | `evaluate_eth_late_cheap_v1` (L841) |
| `finance_experiment` | `finance_updown_v1` | `F` | `experiment_finance_updown_v1_trades.jsonl` | Finance 当日（FIN:SPX/GOLD） | `evaluate_finance_updown_v1` (L909) |
| `crypto_4h_experiment` | `crypto_4h_updown_v1` | `H` | `experiment_crypto_4h_updown_v1_trades.jsonl` | Crypto 4h | `evaluate_crypto_duration_updown_v1` (L1007, daily=false) |
| `crypto_daily_experiment` | `crypto_daily_updown_v1` | `D` | `experiment_crypto_daily_updown_v1_trades.jsonl` | Crypto daily | 同上 (daily=true) |

> 变量名 `trend_experiment` 历史遗留——实际跑的是 `eth_late_cheap_v1`，不是 trend_follow。

---

### 2.1 `eth_cheap_v1`（ETH 便宜便宜的逆势加仓）

`evaluate_eth_cheap_v1` @ L612-694

| 条件 | 规则 |
|---|---|
| 标的 | 仅 ETH |
| 时间窗口 | `minutes_remaining > 30`（无上限） |
| dev 区间 | `0.10 ≤ \|dev\| ≤ 0.24` |
| 方向选择 | UP/DOWN 中 ask 更低的一方 |
| spread | `≤ 0.01` |
| 入场价区间 | `0.20 ≤ entry_price ≤ 0.26` |
| 二级过滤 | `entry > 0.24` → 要求 `\|dev\| ≤ 0.18`（medium_value 桶） |
| 仓位 | medium_value: `$0.50`；其它: `$1.00` |
| 挂单价 | `ask − 0.01`，最低 0.01 |

**TP（`compute_tp_levels`，regime=QUIET_REVERSION）**：

| 档 | 触发价 | 卖比例 |
|---|---|---|
| TP0 | `0.42` | 75% |
| TP1 | `0.62` | 剩余 100% |

**Exit（QUIET_REVERSION 分支，L1186-1198）**：
- `current ≤ entry − 0.07` → `stop_price`
- `\|dev\| > quiet_max_dev + 0.05` → `stop_btc`（dev 偏离扩大）
- `minutes_remaining ≤ 12 AND max_price < 0.42` → `stop_time`

---

### 2.2 `eth_late_cheap_v1`（ETH 末段便宜入场）

`evaluate_eth_late_cheap_v1` @ L841-907

| 条件 | 规则 |
|---|---|
| 标的 | 仅 ETH |
| 时间窗口 | `10 ≤ minutes_remaining ≤ 25` |
| dev 区间 | `0.12 ≤ \|dev\| ≤ 0.28` |
| 方向选择 | UP/DOWN 中 ask 更低的一方 |
| spread | `≤ 0.01` |
| 入场价区间 | `0.20 ≤ entry_price ≤ 0.28` |
| 仓位 | `$0.25`（四分之一仓） |

**TP（`compute_eth_late_cheap_tp_levels`）**：

| 档 | 触发价 | 卖比例 |
|---|---|---|
| TP0 | `min(0.95, entry+0.08)` | 50% |
| TP1 | `min(0.95, entry+0.16)` | 剩余 100% |

**Exit**：`evaluate_eth_late_cheap_exit` @ L1538

---

### 2.3 `finance_updown_v1`（标普 / 黄金 当日 UP/DOWN）

`evaluate_finance_updown_v1` @ L909-1005

| 条件 | 规则 |
|---|---|
| 标的 | `SPX`、`GOLD`（`finance_asset_allowed` @ L62） |
| 时间窗口 | `20 < minutes_remaining ≤ 390`（最长约 6.5h） |
| 题型过滤 | 必须含 "up or down" 且不含 "above/below/over/under"（即排除门槛盘） |
| dev 阈值 | SPX `≥0.35%`、GOLD `≥0.45%`（`finance_dev_threshold`）|
| 数据健全 | dev ≤ 5%（防止 ref 异常） |
| spread | `≤ 0.02` |

**入场两个子模式**：

**(A) 趋势跟随**：
| 条件 | 规则 |
|---|---|
| 时间 | `60 ≤ mr ≤ 330` |
| 方向 | dev 同方向（dev>0 买 UP） |
| 入场价 | `0.55 ≤ entry ≤ 0.72` |
| 仓位 | `$0.50` |

**(B) 极端反转**（趋势条件不满足且 dev 极强时尝试反向）：
| 条件 | 规则 |
|---|---|
| 时间 | `90 ≤ mr ≤ 300` |
| dev | SPX `≥1.00%`、GOLD `≥1.20%`（`finance_extreme_dev_threshold`）|
| 方向 | 反向（dev>0 买 DOWN） |
| 入场价 | `0.18 ≤ entry ≤ 0.30` |
| 仓位 | `$0.25` |

**TP（`compute_finance_updown_tp_levels`）**：

| Regime | TP0 | TP1 |
|---|---|---|
| TREND | `min(0.90, entry+0.08)` 卖 50% | `min(0.92, entry+0.15)` 卖 50% |
| REVERSAL | `min(0.95, entry+0.10)` 卖 60% | `min(0.95, entry+0.20)` 卖 100% |

**Exit**：`evaluate_finance_updown_exit` @ L1583（含 `below_shape` 反向结算逻辑——参见 L1777）

---

### 2.4 `crypto_4h_updown_v1`（Crypto 4 小时 UP/DOWN）

`evaluate_crypto_duration_updown_v1(daily=false)` @ L1007-1070

| 条件 | 规则 |
|---|---|
| 标的 | BTC / ETH / SOL / XRP / BNB |
| 时间窗口 | `60 ≤ minutes_remaining ≤ 210`（约 1-3.5h） |
| dev 阈值 | BTC/ETH `≥0.35%`、其它 `≥0.50%`（`crypto_duration_dev_threshold`） |
| 方向 | dev 同方向 |
| spread | `≤ 0.020` |
| 入场价 | `0.55 ≤ entry ≤ 0.70` |
| 仓位 | `$0.50` |

**TP（`compute_crypto_duration_tp_levels(daily=false)`）**：

| 档 | 触发价 | 卖比例 |
|---|---|---|
| TP0 | `min(0.90, entry+0.08)` | 40% |
| TP1 | `min(0.92, entry+0.15)` | 58% |

**Exit**：`evaluate_crypto_duration_exit(daily=false)` @ L1645

---

### 2.5 `crypto_daily_updown_v1`（Crypto 全日 UP/DOWN）

同 2.4，差异：

| 条件 | 4h | daily |
|---|---|---|
| 标的 | BTC/ETH/SOL/XRP/BNB | BTC/ETH/SOL/BNB（**无 XRP**） |
| 时间窗口 | `60-210` min | `180-900` min（3-15h） |
| dev 阈值（BTC/ETH） | `≥0.35%` | `≥0.60%` |
| dev 阈值（其它） | `≥0.50%` | `≥0.90%` |
| spread | `≤0.020` | `≤0.025` |
| 入场价 | `0.55-0.70` | `0.58-0.72` |
| 仓位 | `$0.50` | `$0.50` |
| TP0 卖比例 | 40% | 35% |
| TP1 触发 | `entry+0.15` 上限 0.92 | `entry+0.16` 上限 0.94 |
| TP1 卖比例 | 58% | 54% |

---

## 三、市场 → engine 分发（main.cpp）

| 市场前缀 / 类型 | 进入的 engine | 代码位置 |
|---|---|---|
| `FIN:*`（标普 / 黄金 / WTI …） | `finance_experiment` | L1258-1329 |
| Crypto 4h 市场 | `crypto_4h_experiment` | L1249-1250 |
| Crypto daily 市场 | `crypto_daily_experiment` | L1252 |
| Crypto 1h（常规） | `experiment` + `trend_experiment`（两个同时跑！）| L1441-1442 |

注意 Crypto 1h 走 **双 engine 并行**——`eth_cheap_v1` 和 `eth_late_cheap_v1` 用同一份 tick 各自评估。

---

## 四、退役 / 代码遗留（不再实例化）

下列策略代码仍在 `experiment_engine.cpp`，但 main.cpp 没实例化，对应历史 jsonl 仅作回放：

| strategy_name | 历史 jsonl | 说明 |
|---|---|---|
| `regime`（默认 `evaluate_entry` L218） | `experiment_trades.jsonl` (14 笔) | 早期三态机（trend/reversal/quiet），现已弃用 |
| `trend_follow` | `experiment_trend_trades.jsonl` (148 笔) | 多币种 trend follow，commit c8d2e8a 加入，5/09 后被替换 |
| `legacy_cheap_v2` | `experiment_legacy_cheap_v2_trades.jsonl` (27 笔) | legacy_cheap 加分制 v2，5/09 commit 8e3f399 后弃用 |
| `eth_only_v1` | `experiment_eth_only_v1_trades.jsonl` (12 笔) | ETH 单币种 trend+cheap 混合，5/12-13 短测 |
| `late_window_v1` | 无 jsonl | BTC/ETH 末 10-20 分钟 trend，从未活跃 |
| `finance_index_v1` | `experiment_finance_index_v1_trades.jsonl` (1 笔) | 旧 finance 命名，被 `finance_updown_v1` 替代 |

历史 jsonl 的分析见 [`prompts/analyze_strategy.md`](../prompts/analyze_strategy.md)；样本 <50 的禁止单独下结论。

---

## 五、风控（所有 experiment 共享）

ExperimentEngine 内置：

| 风控项 | 规则 | 位置 |
|---|---|---|
| 同币种单仓 | `has_open_coin(coin)` 检查，已开则拒 | `on_market` |
| 本场 K 线锁 | 触发 `candle_stopped` 后该 candle 不再开仓 | `CoinRiskState` |
| 每小时 quiet 上限 | `quiet_trades_this_hour_ >= cfg.strategy.max_quiet_trades_per_hour` 时拒 | L1977 |
| 每小时 non-BTC stop_price 上限 | `non_btc_stop_price_this_hour_` 计数 | `record_trade` |
| Live 模式锁币 | live 下只跑 BTC（其它 return） | L1759 |
| 总开关 | `cfg.experiment.enabled` | L1758 |

---

## 六、配套日志字段（jsonl schema）

每笔成交写入对应 `experiment_<name>_trades.jsonl`，关键字段：

`coin / market / regime / side / entry_time / exit_time / entry_price / exit_price / minutes_remaining / hold_duration_sec / size_usdc / shares / fee / pnl / exit_reason / max_price / min_price / mfe_at_5/10/15min / mfe_capture_rate / spread_at_entry / strategy_name / entry_price_bucket / confidence_components`

**注意**：`confidence_components / cross_coin_state / btc_alignment / eth_alignment` 等字段在不同 strategy 下填充率不同，分析时须先验证。详见 `prompts/analyze_strategy.md` 黑名单。

候选（未成交的 reject）写入 `experiment_<name>_candidates.jsonl`（同前缀替换后缀）。

---

## 七、Dashboard 查看

`localhost:9090` Dashboard 提供 dropdown 切换数据 scope：

- `Main`（`logs/trades.jsonl`，主策略）
- `Experiment: eth_cheap_v1`
- `Experiment: eth_late_cheap_v1`
- `Experiment: finance_updown_v1`
- `Experiment: crypto_4h_updown_v1`
- `Experiment: crypto_daily_updown_v1`

每个 scope 独立显示资金曲线、胜率、PnL 分布、exit_reason 分桶、MFE 捕获率等指标。

---

## 八、修改实验策略的工作流

1. 在 `experiment_engine.cpp` 改对应 `evaluate_*` / `evaluate_*_exit` / `compute_*_tp_levels`
2. 如需新策略：加 `evaluate_xxx` + 在 dispatcher（L1942-1971）加分支 + 在 main.cpp 加 `ExperimentEngine` 实例
3. 编译 `cmake --build build -j`
4. 用 `prompts/analyze_strategy.md` 对比新老 jsonl
5. 按 [CLAUDE.md 工作流](../CLAUDE.md)：写 step 报告 → 等 review → commit → 更新 memory
