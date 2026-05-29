# Step — 退役 eth_cheap_v1（含证据）

**日期**：2026-05-30
**结论**：从 main.cpp 摘除实例，停止运行。释放它独占的 `experiment`（变量名）/ `C`（id 前缀）/ `/api/experiment/*`（路由）。策略逻辑（`evaluate_eth_cheap_v1`）按惯例保留在 experiment_engine.cpp 作回放参考，**不要重新实例化**。

---

## 这个策略是什么

ETH-only 逆势 cheap-value：`minutes_remaining > 30` 入场、`0.10 ≤ |dev| ≤ 0.24`、买 UP/DOWN 中较便宜的一侧（入场价 0.20–0.26¢）、宽 TP（0.42¢ 卖 75% / 0.62¢ 清仓）、regime 标签 `QUIET_REVERSION`。变量名历史遗留叫 `experiment`（它是第一个被加入的实验，因此占用了无前缀的 `/api/experiment/*` 路由），id 前缀 `C`。

## ⚠️ 数据来源教训（先记这条）

第一轮分析用的是**本地 `logs/experiment_eth_cheap_v1_trades.jsonl`，只有 15 腿 / 11 仓 / 停在 05-14**，得出 −$0.276 的"温和负"结论——**这是错的**。真实数据在 droplet（159.203.56.165）的同名文件：**317 腿 / 202 仓 / 跑到 05-29**。结论方向一致（负、低胜率、cheap-value 家族）但量级差 40 倍。
**教训**：分析任何还在线上跑的实验前，先 `scp` 服务器日志，本地副本可能严重滞后。

## 为什么退役 —— 数据（服务器全量，2026-05-16 ~ 05-29，202 仓去重）

**总 PnL −$11.84，胜率 37%（75/202），每仓均值 −$0.059。**

| exit_reason | 腿数 | PnL |
|---|---|---|
| stop_price | 81 | **−27.83** |
| no_start_exit | 45 | −8.25 |
| tp0 | 76 | +9.86 |
| trailing_stop | 75 | +6.41 |
| tp1 | 39 | +7.07 |
| expired | 1 | +0.90 |

### 三条独立证据

1. **非肥尾结构 —— 广泛性亏损**。最大单笔 +$2.07，但**剔掉最大 2 笔反而 −$14.88、剔掉最大 3 笔 −$15.85**（即少数赢家根本不是支撑、删掉它们亏得更多）。亏损来自 `stop_price` 持续放血 −$27.83 + `no_start_exit` −$8.25 —— 便宜侧压根不反弹、反而被趋势碾过止损。这与 `eth_late_cheap_v1` 当初"靠 3 笔运气撑"不同，本策略连那点运气都没有。

2. **W22 崩盘，不是平稳负**。按 ISO 周：
   - W20（05-16~17）：33 仓，−$0.59
   - W21（05-18~24）：91 仓，+$0.35
   - **W22（05-25~29）：78 仓，−$11.60**
   05-23 `step-retire-eth-late-cheap-v1.md` 引用它当时"104 笔 +$0.078 41%"作为基准线——那是 W20+W21 的约平状态。一旦进入 05 月末 ETH 急跌（ETH 从 ~2220 跌到 ~2000），逆势抄便宜侧在强趋势/高波动 regime 被直接碾过，单周亏掉全部历史。**它自己当年设的基准线，现在自己跌穿了。**

3. **同族两兄弟已先退役，论点已被反复证伪**：
   - `eth_late_cheap_v1`：−$0.475 / 46 / 28%（2026-05-23 退役）
   - 主策略 `QUIET_REVERSION` / `cheap_rebound`：−$0.51 / 21 / 24%（2026-05-26 退役）
   - `eth_cheap_v1`：−$11.84 / 202 / 37%（本次）
   三者同一论点（0.20–0.28¢ 抄较便宜侧赌反弹），合计约 −$12.8 / 269 仓 / ~35%。这是结构性死结，不是参数问题。

### 为什么不优化而是退役

- 收紧 dev / entry 区间只会缩小已经为负的样本，且 W22 证明问题出在"趋势 regime 下做反转"这个论点本身，不是入场阈值。
- 调 TP / 止损过拟合：盈利不集中在肥尾，无可压缩的亏损尾巴可调；`stop_price` −$27.83 正是"按规则止损仍然亏"，说明方向就是错的。
- 该做"顺趋势"的工作已由 `trend_v2_experiment`（`trend_follow`，+$1.44/77/78%）承担，且它正是吃 ETH 趋势行情的策略——把 Crypto 1h 的 tick 资源全留给它。

## ⚠️ 给未来：不要重蹈的模式

"逆势 cheap-value 失败族"现已**全员退役**（`eth_cheap_v1` / `eth_late_cheap_v1` / `QUIET_REVERSION`）。把以下变体重做成新策略前先回看本文档与 [`step-retire-eth-late-cheap-v1.md`](./step-retire-eth-late-cheap-v1.md)：

- **判据**：任何新 cheap-value 变体上线前，必须先在影子样本里证明**胜率与每笔均值同时优于全族**，且**在一段明确的趋势/高波动 regime 上回测不爆**（W22 就是教科书反例），否则就是本族换皮，直接否决。

## 文件改动

| 改动 | 文件 |
|---|---|
| 删 `experiment` 实例化 + 改退役注释 | `src/main.cpp` (~L445) |
| 删 3 个 callback 注册（status/trades/all_trades） | `src/main.cpp` (~L972) |
| 删 `experiment.reset_candle / reset_global_hour / on_market / prune_closed` 调用 | `src/main.cpp` |
| 删 `on_experiment_status/trades/all_trades` 声明 | `src/net/api_server.h` |
| 删对应 cb 成员 + setter 定义 + `/api/experiment/*` 路由 | `src/net/api_server.cpp` |
| 删 "ETH Cheap" tab 按钮 + 面板 + persistDetails + fetch + render 引用 | `src/dashboard.h` |
| 退役注释（休眠，作回放） | `src/core/experiment_engine.cpp` 的 `evaluate_eth_cheap_v1` |
| 总表 5→4 + 退役注 + 失败族全员退役注 + §2.1 标退役 + 路由表单 engine + 退役表 + dashboard scope | `docs/strategy-experiments.md` |

## 验证

- `cmake --build build -j` 通过，无 warning。
- `grep` 确认 main.cpp 无残留 bare `experiment.`、dashboard.h 无残留 `eth-cheap`/`/api/experiment/`（除共享 `renderExperiment` 形参）。
