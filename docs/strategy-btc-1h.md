# Polymarket 1小时 UP/DOWN 交易策略

> 最后更新：2026-05-17（branch `experimental-simulator`，commit 0f827fb 入场时间窗收紧到 mr < 45）。
> 当前主策略 `regime_adaptive` 由 `src/core/strategy.cpp` 提供：先按 `abs(dev)` 拆 quiet / uncertain / momentum 三态，再走两个独立 sub-strategy（`cheap_rebound` / `momentum_follow`）；中间不确定区间不交易。
> 实验/影子策略另见 [`strategy-experiments.md`](./strategy-experiments.md)。

---

## 一、市场类型

- **标的**：Polymarket Crypto 1 小时 UP/DOWN 二元期权
- **监听币种**：默认 `BTC / ETH / SOL / XRP / DOGE / BNB`（`StrategyConfig::crypto_symbols` 默认值，`config.json` 的 `coins` 段可覆盖）
- **live 交易币种**：只允许 BTC（live 启动守卫强制，main.cpp R-V2.3）
- **dry run 交易币种**：允许多币种并行（每币种独立单仓）
- **合约价格区间**：$0 - $1
- **到期结算**：胜方结算 $1，败方结算 $0

---

## 二、核心策略：regime_adaptive

`Strategy::evaluate_entry`（strategy.cpp L312-339）按 BTC 当前小时 `abs(dev)` 分流：

| Regime | 条件 | 路由 |
|---|---|---|
| quiet | `abs(dev) ≤ 0.12%` | `evaluate_cheap_rebound` |
| uncertain | `0.12% < abs(dev) < 0.20%` | reject `regime_uncertain` |
| momentum | `abs(dev) ≥ 0.20%` | `evaluate_momentum_follow` |

### 2.1 cheap_rebound 入场条件（L77-245）

| 条件 | 当前规则 |
|---|---|
| 支持币种 | **仅 BTC、ETH**（SOL/XRP/DOGE/BNB 直接 reject `cheap_rebound_coin_filter`） |
| 时间窗口 | `30 < minutes_remaining < 45`（即 31..44；commit 0f827fb 上限 50→45）|
| 方向选择 | UP/DOWN 中 ask 更低的一方 |
| ask 红线 | BTC `≤ 0.29`，ETH `≤ 0.31` |
| 入场价区间 | BTC `0.24 ≤ entry ≤ 0.26`；ETH `0.24 ≤ entry ≤ 0.30` |
| spread 过滤 | `≤ 0.01` |
| dev 下限 | `abs(dev) ≥ 0.10` |
| dev 上限（BTC） | `≤ 0.12` |
| dev 上限（ETH，按 entry 三段） | entry ≤ 0.24 → 0.24；entry ∈ (0.24, 0.26] → 0.18；entry ∈ (0.26, 0.30] (medium) → 0.24 |
| **ETH medium 特例** | entry > 0.26 时额外要求 `mr ≥ 35` AND `abs(dev) ≥ 0.18`，仓位减半到 `$0.50` |
| 跨币种背景 | 观察其它币 ≥3 个且 (反向 ≥3 或 核心反向 ≥2) → reject `cheap_rebound_cross_coin_opposed` |
| 入场置信度 | `confidence ≥ 4`（评分项见 2.3） |
| 高价二次过滤 | `entry ≥ 0.27` 且非 ETH medium → 要求 `confidence ≥ 5` |
| 挂单价格 | `entry = candidate_ask - 0.01`，最低 0.01 |
| 单笔成本 | ETH medium `$0.50`，否则 `$1.00` |
| 同币种持仓 | 由 main.cpp 控制：同币种未平仓则跳过 |
| live 限制 | live 模式只交易 BTC（启动守卫） |

### 2.2 momentum_follow 入场条件（L247-310）

| 条件 | 当前规则 |
|---|---|
| 支持币种 | 除 SOL 外全部（`if (coin_ == "SOL") reject`） |
| 时间窗口 | `25 ≤ minutes_remaining < 45`（即 25..44；commit 0f827fb 上限 50→45）|
| 方向选择 | `dev > 0` 买 UP，`dev < 0` 买 DOWN（追强边）|
| dev 阈值 | `abs(dev) ≥ 0.20%` |
| spread 过滤 | `≤ 0.01` |
| 入场价区间 | `0.64 ≤ entry ≤ 0.67` |
| 非 BTC 加严 | `coin != BTC` 时要求 `abs(dev) ≥ 0.24%` |
| 挂单价格 | `entry = ask - 0.01` |
| 单笔成本 | `$1.00` |
| 置信度评分 | **无**（momentum 不评 confidence） |

### 2.3 cheap_rebound confidence 评分（共 9 条）

| 加/扣分 | 组件名 | 触发 |
|---|---|---|
| +1 | time_41_45 | `41 ≤ mr ≤ 45` |
| +1 | spread_tight | 入场时即满足 spread ≤ 0.01（恒为真，常加分） |
| +1 | entry_value_22_26 | `0.22 ≤ entry < 0.27` |
| +1 | eth_medium_entry_27_30 | ETH medium：entry ∈ (0.26, 0.30] |
| +1 | core_coin | coin ∈ {BTC, ETH, SOL} |
| 0 | non_core_coin | 否则 |
| +1 | dev_sweet_spot | `abs(dev) ∈ [核心 0.08 / 非核心 0.05, max_abs_dev]` |
| -1 | dev_too_weak | `abs(dev) < 甜区下限` |
| -1 | entry_high_27_plus | `entry ≥ 0.27` 且非 ETH medium |
| -1 | cross_coin_some_opposed | 跨币种背景观察到任一反向 |

> 已知缺陷：confidence 评分组件目前**没落到 jsonl 日志**（填充率 0%）。回溯调优需先把 `confidence_components` 字段写盘——见 [`prompts/analyze_strategy.md`](../prompts/analyze_strategy.md) 黑名单。

### 2.4 spread 与入场价说明

- `spread = ask - bid`。spread 过宽时按 bid 估值会出现天然浮亏，因此入场强制 `spread ≤ 0.01`
- 低价不等于便宜：历史数据显示 `entry < 0.10` 的合约多数是已经接近归零的一侧。当前 cheap_rebound 最低 0.24，从源头规避

---

## 三、仓位与风控

### 3.1 仓位

- 单笔目标投入：cheap_rebound 通常 `$1.00`，ETH medium 减半到 `$0.50`；momentum `$1.00`
- shares 按 `size_usdc / entry_price` 计算
- dry run 可多币种并行持仓，但**禁止同一币种重复持仓**
- live 模式：`max_concurrent = 1` 严格单仓（risk_manager L11），且仅 BTC

### 3.2 同币种本场 K 线锁仓

任意以下 exit_reason 触发后，该币种**本 1h K 线**不再开仓（main.cpp L1900-1905）：

| exit_reason | 说明 |
|---|---|
| `stop_price` | 通用价格止损 |
| `stop_time` | 末段时间止损 |
| `stop_btc` | BTC 动量衰减 / dev 偏离扩大 |
| `trailing_stop` | 移动止盈回撤 |
| `dead_water_exit` | 死水早退 |
| `fast_fail_exit` | 入场后 180s 未启动 |
| `cheap_fail_stop` | QUIET 分支 90s 未启动 |
| `adverse_expansion_stop` | QUIET 分支不利扩张 |

新 K 线开始时通过 `reset_candle` 解锁。

### 3.3 全局熔断（本小时）

| 触发 | 阈值 | 效果 |
|---|---|---|
| 总 `stop_price` 次数 | `≥ 3` | 停所有新开仓（risk_manager L29） |
| 非 BTC `stop_price` 次数 | `≥ 3` | 停所有非 BTC 新开仓（L23） |

新小时开始通过 `reset_global_hour` 重置。

---

## 四、止盈规则（`compute_tp_levels`，L341-364）

按 entry_price 分两套（不是按 regime label）：

### entry < 0.60（cheap_rebound 入场区间）

| 档位 | 触发价 | 卖出比例（剩余仓位） |
|---|---|---|
| TP0 | `0.45` | 75% |
| TP1 | `0.70` | 50% |
| TP2 | `0.88` | 100% |

### entry ≥ 0.60（momentum 入场区间）

| 档位 | 触发价 | 卖出比例（剩余仓位） |
|---|---|---|
| TP0 | `min(0.88, entry + 0.08)` | 50% |
| TP1 | `min(0.90, entry + 0.14)` | 50% |
| TP2 | `0.90` | 100% |

说明：
- TP0 先锁定大部分已到手利润；TP1 卖剩余的一半；TP2 清空尾仓
- live 模式 TP 使用 FAK @ 0.01 floor，按真实成交 shares 和均价记账

---

## 五、退出规则

退出按 regime 分两个独立分支（先各自检查），再走通用规则。优先级从上到下。

### 5.1 TREND 分支专属（L378-462，仅 momentum 入场触发）

| 触发 | 条件 | exit_reason |
|---|---|---|
| 严格止损 | `current ≤ entry - 0.07` | `stop_price` |
| 动量衰减 | UP 仓位 `dev < entry_dev - 0.08`；DOWN 仓位 `dev > entry_dev + 0.08` | `stop_btc` |
| 启动失败 | `elapsed ≥ 150s` AND `mfe < 0.03` AND `current ≤ entry - 0.03` | `no_start_exit` |
| TP 后保护 | has_tp AND `current ≤ entry + 0.02` | `trailing_stop` |
| trailing (mfe≥0.12) | `current ≤ max(entry+0.05, max_price-0.04)` | `trailing_stop` |
| trailing (mfe≥0.08) | `current ≤ max(entry+0.02, max_price-0.05)` | `trailing_stop` |
| 末段 5min | `mr ≤ 5` AND (`current < 0.88` 或 dev 已反向) | `stop_time` |
| 末段 8min | `mr ≤ 8` AND (`current < 0.78` 或 dev 已反向) | `stop_time` |

### 5.2 QUIET_REVERSION 分支专属（L465-499，仅 cheap_rebound 入场触发）

| 触发 | 条件 | exit_reason |
|---|---|---|
| 启动失败 | `elapsed ≥ 150s` AND `mfe < 0.03` AND `current ≤ entry - 0.03` | `no_start_exit` |
| cheap 失败 | `elapsed ≥ 90s` AND `mfe < 0.02` AND `current ≤ entry - 0.03` | `cheap_fail_stop` |
| 不利扩张 | `elapsed ≥ 180s` AND `max_adverse ≥ 0.06` AND `mfe < 0.10` AND `current ≤ entry - 0.05` | `adverse_expansion_stop` |

### 5.3 通用退出（所有 regime，L502-592）

未被 5.1 / 5.2 拦住的仓位继续走：

| 触发 | 条件 | exit_reason |
|---|---|---|
| 价格止损 | `loss_pct ≥ 30%` | `stop_price` |
| 快速失败 | `elapsed ≥ 180s` AND `mfe < 0.02` AND `current ≤ entry - 0.03` | `fast_fail_exit` |
| TP 后保护 | has_tp AND `current ≤ entry + 0.02` | `trailing_stop` |
| 死水早退 | `elapsed ≥ 480s` AND `mfe < 0.02` AND `current ≥ entry × 0.85` | `dead_water_exit` |
| trailing (mfe≥0.15) | `current ≤ max(max_price - 0.07, entry + 0.08)` | `trailing_stop` |
| trailing (mfe≥0.10) | `current ≤ max(max_price - 0.08, entry + 0.04)` | `trailing_stop` |

### 5.4 最后 10 分钟（`evaluate_last_10min`，L602-625）

| current 价格 | 处理 |
|---|---|
| `< 0.25` | `stop_time` 立即清仓 |
| `0.25 - 0.80` | 不强制退出，继续按 5.3 规则 |
| `≥ 0.80` | 持有到期博 $1 结算 |

---

## 六、live 与 dry run 差异

| 项目 | dry run | live |
|---|---|---|
| 交易币种 | 多币种 | BTC only（启动守卫） |
| 真实下单 | 否 | 是 |
| 入场 | 模拟 maker limit | 真实 GTC maker（fee 0%），等待 fill |
| TP 出场 | 模拟 best bid | FAK @ 0.01 floor（taker，fee 7.2%），按真实成交记账 |
| 止损 / 撤退 | 模拟 best bid | FOK @ 0.01 floor（taker，fee 7.2%），按真实成交记账 |
| 同币种重复持仓 | 禁止 | 单仓制 `max_concurrent=1` 更严 |
| 余额来源 | 虚拟 `account_balance`（risk_manager） | 真实 vault pUSD（live_trader 缓存） |

> 注：fee 结构来自 `config.json` `fees` 段（maker 0%、taker 7.2%）。所有 exit 走 taker 是当前最大成本中心，详见 `docs/steps/step-strategy-mr-upper-45.md`。

---

## 七、实验策略

详见 [`strategy-experiments.md`](./strategy-experiments.md)。当前与主策略并行运行 5 个 shadow 实验（`eth_cheap_v1` / `eth_late_cheap_v1` / `finance_updown_v1` / `crypto_4h_updown_v1` / `crypto_daily_updown_v1`），各自独立日志、独立 $20 起始余额，不影响主策略也不下真单。

---

## 八、关键复盘结论（historical，2026-04-21 ~ 2026-05-17）

按 commit 时间窗切片的"修复后"基线（commit 0f827fb 之前 R2+R3 数据）：

| 段 | 时间 | 笔数 | 净 PnL | wr | 备注 |
|---|---|---|---|---|---|
| 主策略 R0_baseline | 04-21 ~ 05-03 | 140 | +0.03 | 43.6% | 单仓制时代，fee 几乎吃光毛利 |
| 主策略 R1_unlimited（事故）| 05-04 ~ 05-05 | 211 | **−28.98** | 33.2% | "Allow unlimited dry-run trading" commit 后，必须从历史回放剔除 |
| 主策略 R2 恢复单仓 | 05-06 ~ 05-08 | 54 | +0.47 | 51.9% | 修复后基线 |
| 主策略 R3 + confidence | 05-09 ~ 05-13 | 40 | −2.07 | 47.5% | confidence gates 反而让 PnL 由正转负，待审计 |

主要亏损来源：
- **fee 主导**：gross PnL 仅 −6 美金，但 fee 24.65 美金，fee 是 gross 亏损的 4 倍
- **stop_price 占退出原因 45%**（202/446 笔），平均单笔 −0.68
- **45-60 min 入场子集**：在 R2+R3 段贡献 −6.5 美金，正是 commit 0f827fb 把 mr 上限收紧到 45 的依据

完整分析方法见 [`prompts/analyze_strategy.md`](../prompts/analyze_strategy.md)。

---

## 九、绝对红线（按代码事实，2026-05-17）

1. cheap_rebound 入场价：BTC `0.24-0.26`，ETH `0.24-0.30`；不超过此范围
2. momentum 入场价：`0.64-0.67`；不超过此范围
3. spread 严格 `≤ 0.01`（1¢）
4. 入场时间窗：cheap `31-44 min`，momentum `25-44 min`（commit 0f827fb 上限 50→45）
5. cheap_rebound 仅 BTC + ETH；momentum 不交易 SOL
6. 单仓制：同币种持仓未清前不再开该币种
7. 任意 stop 类退出（见 §3.2 共 8 个 exit_reason）后，**本币种本 K 线** 锁仓
8. **本小时** 全局 `stop_price ≥ 3` 停所有新开仓
9. **本小时** 非 BTC `stop_price ≥ 3` 停所有非 BTC 新开仓
10. live 模式只允许 BTC（启动守卫强制）
11. 私钥永不进日志（`PrivateKey` RAII secure_zero）

---

> 风险提示：以上是程序当前实现的策略规则，不构成投资建议。dry run 数据只能用于观察策略行为，不能保证 live 成交质量和收益。
