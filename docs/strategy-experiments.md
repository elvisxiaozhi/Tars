# 实验策略（Experiment Strategies）

> 最后更新：2026-06-07（branch `experimental-simulator`）。本文档描述**与主策略并行运行的 7 个 shadow 实验策略**，它们只跑模拟、不下真单，用于评估"如果换成 X 策略会怎样"。
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

## 二、当前活跃的 4 个实验策略

### 总表（按 main.cpp 实例化顺序）

| 变量名 | strategy_name | id 前缀 | 日志文件 | 市场类型 | 评测代码 |
|---|---|---|---|---|---|
| `finance_experiment` | `finance_updown_v1` | `F` | `experiment_finance_updown_v1_trades.jsonl` | Finance 当日（FIN:SPX/GOLD） | `evaluate_finance_updown_v1` (L909) |
| `crypto_4h_experiment` | `crypto_4h_updown_v1` | `H` | `experiment_crypto_4h_updown_v1_trades.jsonl` | Crypto 4h | `evaluate_crypto_duration_updown_v1` (L1007, daily=false) |
| `crypto_daily_experiment` | `crypto_daily_updown_v1` | `D` | `experiment_crypto_daily_updown_v1_trades.jsonl` | Crypto daily | 同上 (daily=true) |
| **`trend_v2_experiment`** | `trend_follow` | `T` | `experiment_trend_v2_trades.jsonl` | Crypto 1h（仅 BTC/ETH/BNB） | `evaluate_trend_follow` (L329) |

> **⚠️ 2026-06-10 退役 `trend_v3`(V) + `held_favorite_v1`(K) + `held_momentum_v1`(M) —— "买顺势 favorite 赌延续"动量族无 edge。** 三者与 main/trend_v2 同属一个赌。真实费 era 数据(各跑数天)：全员净负,且**毛 edge≈0**——带管理的 trend_v2 毛 +$0.66/124 笔(≈每笔 0)、main/v3 毛微负;纯持有的 held_momentum 直接负:**favored 侧 hold-to-expiry 实测只赢 43%,远低于入场价 0.60(EV/股≈−0.17)**。结论:这一族买在**市场隐含概率**上,结构性零 edge,再生变体不会变出 edge。另:`held_favorite` 因 bot 在距收盘 ~20min 即切走当前小时盘(数据下限 mr≈20),其 2–12min 窗口**永远触发不了**(0 成交)。三者 `evaluate_*` 代码按惯例保留在 experiment_engine.cpp 作回放参考,**不再实例化,dashboard tab + API 路由一并移除**。证据见 [`steps/step-retire-held-and-trendv3.md`](./steps/step-retire-held-and-trendv3.md)。**❌ 不要再以"买顺势 favorite / 持有到期"的任何变体重做这一族。**
>
> （退役前设计存档:held 双策略 [`steps/step-held-to-expiry-shadows.md`](./steps/step-held-to-expiry-shadows.md);trend_v3 [`steps/step-trend-v3-shadow.md`](./steps/step-trend-v3-shadow.md)。）

> **2026-05-30 退役 `eth_cheap_v1`**（原变量名 `experiment`，id 前缀 `C`，原占用 `/api/experiment/*` 路由）。逆势 cheap-value 失败族**第 3 例**，全量服务器日志（202 仓 / 05-16~05-29）净 **−$11.84 / 37% 胜率 / 每仓 −$0.059**。**非肥尾结构**——剔掉最大 2 笔反而 −$14.88，是 `stop_price` 持续放血（81 腿 −$27.83）。05-23 时还约平（+$0.078），W22（05-25~29 ETH 急跌段）单周崩 −$11.60，逆势抄便宜侧在趋势/高波动 regime 被直接碾过。与已退役的 `eth_late_cheap_v1` / `QUIET_REVERSION` 同族同结构，无可改杠杆。证据见 [`steps/step-retire-eth-cheap-v1.md`](./steps/step-retire-eth-cheap-v1.md)。dashboard 的 "ETH Cheap" tab + `/api/experiment/*` 路由一并移除。

> **2026-05-23 退役 `eth_late_cheap_v1`**（原变量名 `trend_experiment`，id 前缀 `L`）。净负、无可改杠杆。证据与"不要重蹈的模式"见 [`steps/step-retire-eth-late-cheap-v1.md`](./steps/step-retire-eth-late-cheap-v1.md)。dashboard 的 "Trend Follow" tab 一并移除。
> 真正的 `trend_follow` 在 `trend_v2_experiment` 中跑——2026-05-17 复活，基于历史 148 笔数据分析（详见 [`steps/step-trend-v2-revive.md`](./steps/step-trend-v2-revive.md)）显示其当前 code 在排除事故段后实际为 +$1.44 / 77 笔 / wr 78%。

> **2026-05-26 退役主策略 `QUIET_REVERSION` / `cheap_rebound`**（不是实验、是主策略 regime；这里只作交叉记录）。它是 `eth_late_cheap_v1` 的早窗亲兄弟（同 `QUIET_REVERSION` 标签、同"抄便宜侧赌反弹"），同属"逆势 cheap-value"失败族，净 −$0.51/21 仓/24%。上面那条"不要重蹈的模式"判据同样适用。证据见 [`steps/step-retire-quiet-reversion.md`](./steps/step-retire-quiet-reversion.md)。

> ⚠️ **"逆势 cheap-value 失败族"已全员退役**（`eth_cheap_v1` −$11.84/202、`eth_late_cheap_v1` −$0.475/46、`QUIET_REVERSION` −$0.51/21，合计约 **−$12.8 / 269 仓 / ~35% 胜率**）。三者共享同一论点——在 0.20–0.28¢ 抄较便宜侧赌反弹。**不要再以任何变体重做这一族**，便宜侧不反弹是结构性死结、无杠杆可改，且在趋势/高波动 regime（如 05 月末 ETH 急跌）会被放大成大额 `stop_price` 亏损。新 cheap-value 变体上线前必须先在影子样本里证明胜率与每笔均值同时优于全族，否则直接否决。

---

### 2.1 `eth_cheap_v1`（ETH 便宜侧逆势加仓）—— ⚠️ 2026-05-30 已退役

> **已退役，不再实例化。** 逆势 cheap-value 失败族第 3 例：全量 202 仓净 −$11.84 / 37% 胜率 / 每仓 −$0.059，非肥尾结构（剔掉最大 2 笔反而更亏）；`stop_price` 81 腿放血 −$27.83、`no_start_exit` 45 腿 −$8.25（便宜侧压根不反弹）。05-23 还约平，W22 ETH 急跌段单周崩 −$11.60。
> 退役证据见 [`steps/step-retire-eth-cheap-v1.md`](./steps/step-retire-eth-cheap-v1.md)。
> 以下规则仅作历史记录；`evaluate_eth_cheap_v1` 代码按惯例保留在 experiment_engine.cpp 作回放参考，**不要重新实例化**。

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

### 2.2 `eth_late_cheap_v1`（ETH 末段便宜入场）—— ⚠️ 2026-05-23 已退役

> **已退役,不再实例化。** 净负(−$0.475/46笔/28%胜率)、被 `eth_cheap_v1` 严格压制、无可改杠杆。
> 退役证据 + "不要重蹈的模式" 见 [`steps/step-retire-eth-late-cheap-v1.md`](./steps/step-retire-eth-late-cheap-v1.md)。
> 以下规则仅作历史记录;`evaluate_eth_late_cheap_*` 代码按惯例保留在 experiment_engine.cpp 作回放参考。

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

### 2.6 `trend_follow` v2 复活（`trend_v2_experiment`）

`evaluate_trend_follow` @ L329-461

| 条件 | 规则 |
|---|---|
| 支持币种 | 仅 BTC / ETH / BNB |
| 时间窗口 | `41 ≤ minutes_remaining ≤ 45`（很窄，每 candle 仅 5min 入场窗口）|
| 方向选择 | dev 同方向追强边 |
| **方向确认** | 上一 tick 同向（`direction_confirmed`），防假突破 |
| dev 阈值 | `abs(dev) ≥ 0.18%` |
| spread | `≤ 0.01` |
| ask 区间 | `0.65 ≤ ask ≤ 0.68` |
| 入场价 | `0.64 ≤ entry ≤ 0.67`（= ask − 0.01） |
| confidence 评分 | 9 条 components，必须 `≥ 4`（详 confidence_components 字段） |
| 仓位 | DOWN `$1.25` / UP `$1.00` |

**TP（`compute_trend_follow_tp_levels` @ L1089）**：

| 档 | 触发价 | 卖比例 |
|---|---|---|
| TP0 | `min(0.90, entry+0.10)` | 50% |
| TP1 | `min(0.90, entry+0.15)` | 50% |
| TP2 | `0.90` | 100% |

**Exit（`evaluate_trend_follow_exit` @ L1285）**：

| 触发 | 条件 | exit_reason |
|---|---|---|
| 严格止损 | `current ≤ entry − 0.08` | `stop_price` |
| 动量衰减 | `\|dev − entry_dev\| ≥ 0.08` 反向 | `stop_btc` |
| 启动失败 | `elapsed≥150s` AND `mfe<0.03` AND `current≤entry-0.03` | `no_start_exit` |
| 跨币种反向 | BTC/ETH 与持仓方向 opposed 且未浮盈 | `no_start_exit` |
| TP 后保护 | has_tp AND `current ≤ entry + 0.02` | `trailing_stop` |
| 死水 | `elapsed≥480s` AND `mfe<0.03` | `dead_water_exit` |
| trailing (mfe≥0.12) | `current ≤ max(entry+0.05, max-0.04)` | `trailing_stop` |
| trailing (mfe≥0.08) | `current ≤ max(entry+0.02, max-0.05)` | `trailing_stop` |
| 末段 5min | `mr≤5` + (`current<0.88` 或 dev 反向) | `stop_time` |
| 末段 8min | `mr≤8` + (`current<0.78` 或 dev 反向) | `stop_time` |

**复活依据**：历史 `experiment_trend_trades.jsonl` 148 笔表面亏 −$4.11，但分析（按 commit 时间窗切片）显示损失 100% 来自 R2 段 (2026-05-07~05-08) 的过松配置（mr 无上限 / dev≥0.12 / 含 SOL/XRP/DOGE）。当前 code 已通过 mr 41-45、dev≥0.18、BTC/ETH/BNB only、direction_confirmed、confidence≥4 等过滤封堵了所有 R2 漏洞。反事实回放（排除 R2 + 仅 BTC/ETH/BNB）= **+$1.44 / 77 笔 / wr 78%**。

详细分析见 `docs/steps/step-trend-v2-revive.md`。

### 2.7 `trend_v3`（dev-accel 门解锁高价带）—— 2026-06-02 新增

`evaluate_trend_v3` @ experiment_engine.cpp（trend_follow 之后）

合并 main + trend_v2 各自最优 + 「dev-accel 门」。**核心假设：用 ~90s dev 上升门筛出"会跟随"的动量，把被 main 砍掉的高价带（0.68-0.79）从负 EV 救成正 EV**，提高赚钱量。纯 shadow。

| 条件 | 规则 |
|---|---|
| 标的 | 仅 BTC |
| 时间窗口 | `40 ≤ minutes_remaining ≤ 45` |
| 方向确认 | 上一 tick 同向（承自 trend_v2，1-tick） |
| dev 阈值 | `abs(dev) ≥ 0.20%` |
| spread | `≤ 0.01` |
| **入场带（分层）** | `0.64–0.67`：免门；`0.68–0.79`：**必须过 dev-accel 门** |
| **dev-accel 门** | ~90s 窗口内 `abs_dev` 上升 `> 0.01`（`tv3_dev_hist_` deque 维护，用 `now_ms`） |
| 仓位 | `$1.00`（平，不抄 v2 的 DOWN $1.25，避免 size 混淆） |

**TP / Exit**：复用 trend_v2 的 `compute_trend_follow_tp_levels` / `evaluate_trend_follow_exit`。

**回测依据**：对 main 58 笔 BTC momentum，从 `main_candidates.jsonl` 逐 tick 重建入场前 ~90s dev 轨迹（按 coin=BTC + ts 窗口匹配，逐笔验证过）。dev 上升 **63%** vs 不升 **35%**（z≈1.9）；门控高价 E≥0.70 从 50%/−EV 劈成 accel **64%/+EV** vs not-accel 31%。in-sample、n 偏小 → shadow 出样本确认。落盘 `entry_price_bucket` 分 `cheap_0.64_0.67` / `high_gated_0.68_0.79`，`confidence_components` 记 `dev_rising/dev_flat/accel_warmup`。成功判据 = `high_gated` 桶复现 ~64%/+EV。

详见 `docs/steps/step-trend-v3-shadow.md`。

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
| Crypto 1h（常规） | `trend_v2_experiment` + `trend_v3_experiment`（并行双 engine）| `trend_v2_experiment.on_market` / `trend_v3_experiment.on_market` 相邻两行 |

注意 Crypto 1h 现在由 `trend_v2`（`trend_follow`）+ `trend_v3` 两个 engine 并行评估同一份 tick（2026-06-02 加 trend_v3）。两者独立持仓/jsonl，互不影响。曾经的旧双 engine（`eth_cheap_v1` + `trend_follow`）已随 `eth_cheap_v1` 于 2026-05-30 退役；`eth_late_cheap_v1` 更早于 2026-05-23 退役。

---

## 四、退役 / 代码遗留（不再实例化）

下列策略代码仍在 `experiment_engine.cpp`，但 main.cpp 没实例化，对应历史 jsonl 仅作回放：

| strategy_name | 历史 jsonl | 说明 |
|---|---|---|
| `eth_cheap_v1` | `experiment_eth_cheap_v1_trades.jsonl` (202 仓) | **2026-05-30 退役**。逆势 cheap-value 失败族第 3 例，净 −$11.84/37% 胜率/非肥尾，W22 ETH 急跌段单周崩 −$11.60。详 [`steps/step-retire-eth-cheap-v1.md`](./steps/step-retire-eth-cheap-v1.md) |
| `eth_late_cheap_v1` | `experiment_eth_late_cheap_v1_trades.jsonl` (46 笔) | **2026-05-23 退役**。净负/无杠杆。详 [`steps/step-retire-eth-late-cheap-v1.md`](./steps/step-retire-eth-late-cheap-v1.md) |
| `regime`（默认 `evaluate_entry` L218） | `experiment_trades.jsonl` (14 笔) | 早期三态机（trend/reversal/quiet），现已弃用 |
| ~~`trend_follow`~~ | `experiment_trend_trades.jsonl` (148 笔) | **2026-05-17 已复活**，新 jsonl `experiment_trend_v2_trades.jsonl`。历史日志保留用作回放参考 |
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
- `Experiment: finance_updown_v1`
- `Experiment: crypto_4h_updown_v1`
- `Experiment: crypto_daily_updown_v1`
- `Experiment: trend_v2` (`Trend Follow v2` 面板)
- `Experiment: trend_v3` (`Trend Follow v3` 面板，dev-accel 门解锁高价带)

每个 scope 独立显示资金曲线、胜率、PnL 分布、exit_reason 分桶、MFE 捕获率等指标。

---

## 八、修改实验策略的工作流

1. 在 `experiment_engine.cpp` 改对应 `evaluate_*` / `evaluate_*_exit` / `compute_*_tp_levels`
2. 如需新策略：加 `evaluate_xxx` + 在 dispatcher（L1942-1971）加分支 + 在 main.cpp 加 `ExperimentEngine` 实例
3. 编译 `cmake --build build -j`
4. 用 `prompts/analyze_strategy.md` 对比新老 jsonl
5. 按 [CLAUDE.md 工作流](../CLAUDE.md)：写 step 报告 → 等 review → commit → 更新 memory
