# Step — 退役 QUIET_REVERSION / cheap_rebound（主策略，含证据）

**日期**：2026-05-26
**结论**：主策略不再开 QUIET_REVERSION（`cheap_rebound`）仓。`evaluate_entry` 仍调用 `evaluate_cheap_rebound` 以保留 `main_candidates.jsonl` 候选/回放数据，但开关 `strategy.quiet_reversion_enabled`（默认 `false`）关闭时把本应成交的信号改判 `quiet_reversion_retired`。**代码休眠保留作回放，不要重新启用上实盘。**

---

## 这个策略是什么

主策略 `regime_adaptive` 的 quiet 分支：`abs(dev) ≤ 0.12%` 时走 `evaluate_cheap_rebound`（strategy.cpp L78-245）。BTC/ETH、入场窗 mr 31-44、选 ask 更便宜的一侧、买入 0.24-0.26（ETH 到 0.30）、spread ≤ 1¢、confidence ≥ 4，赌近平值市场的"便宜侧"反弹。position 打 `StrategyRegime::QUIET_REVERSION` 标签。

## 为什么退役 —— 数据（trades.jsonl，2026-05-16 ~ 05-25，仓位级去重）

主策略 dry_run 全周期 **+$0.041 几乎打平**，拆 regime 后利润全来自 momentum：

| regime | 真实仓位 | 总 PnL | 胜率 |
|---|---|---|---|
| **momentum (trend)** | 22 | **+$0.551** | 19/22 (86%) |
| **QUIET_REVERSION** | 21 | **−$0.510** | 5/21 (24%) |

QR 仓位级明细：5 笔盈利合计 +$3.21（TP0 从 0.25 涨到 0.45，单笔 +0.58~0.97），16 笔亏损合计 −$3.72。每仓期望 ≈ **−$0.024 / $1**。

### 三条独立证据

1. **与已退役的 `eth_late_cheap_v1` 同族同标签**。两者在代码里都写 `sig.regime = StrategyRegime::QUIET_REVERSION`，同一套"选便宜侧、spread≤1¢、赌反弹"机制，只差时间窗（早窗 mr 31-44 vs 晚窗 mr 10-25）、币种、仓位大小。晚窗那只 2026-05-23 已以"净负 + 无可改杠杆"退役（−$0.475/46 笔/28%）。早窗这只负 edge 量级一致（−$0.51/21/24%），是同一失败模式。

2. **是全部回撤的来源**。退役后 dry PnL 由 +$0.04 → +$0.55（剩 momentum 单独），同时去掉了主要回撤与高方差。利润形态健康（86% 胜率）的引擎被单独保留。

3. **入场无 edge，止损已尽力**。05-21 加的 `qr_breakeven_trail` + `adverse_expansion_stop` 已让单笔亏损变小（近期 −$0.05/−0.17/−0.015 vs 早期 −$0.31/−0.51），说明**出场在生效、问题在入场**——"抄便宜货赌反弹"在方向上与真正赚钱的 momentum（追势）相反，数据说接飞刀这边不赚钱。

## 置信度声明（诚实标注）

- **"QR 当前不是正贡献"——高把握（~85%）**：净负、24% 仓位胜率、是全部回撤来源，无任何正期望证据。
- **"统计上确证负 edge"——中等把握（~65%）**：21 仓 / 10 天小样本，−$0.51 高度依赖 05-17（+$1.50）、05-22（+$1.03）两个好日子；抽掉即深亏，多来两个那种日子又能翻正。严格说是"小样本、轻微为负、高方差"。

退役依据**不是**"证明它必亏"，而是：(a) 找不到 edge，(b) 同门 `eth_late_cheap_v1` 已在同赛道退役、无反超机制，(c) 高方差却无对应回报，(d) 盈利依赖少数离群仓。

## ⚠️ 重要联动：退役会放大"低波动日 0 成交"

退役后主策略**只在 `abs(dev) ≥ 0.20%` 入场**（quiet 退役 + uncertain 区本就不交易）。QR 本是平静日唯一还在开仓的东西（且在亏），所以 2026-05-25 "跑一天 0 成交" 的现象退役后会更常见。这是**有意取舍**：用频率换掉负 edge。若要恢复频率，正确方向是改 momentum 入场（0.64-0.67 区间过窄，近 48h 趋势候选 100% 实际价 <0.55）或填 0.12-0.20 死区，**而非复活 QR**。

## ⚠️ 给未来：不要重蹈的模式

沿用 `strategy-experiments.md` 与 `step-retire-eth-late-cheap-v1.md` 的判据。任何"逆势 cheap-value / 抄便宜侧赌反弹"的新变体上线前，必须先在影子样本里证明**胜率 > momentum 的 ~86%** 或**每笔均值 > momentum**，否则就是本策略的换皮，直接否决。

## 文件改动

| 操作 | 文件 |
|------|------|
| 加开关字段 `quiet_reversion_enabled`（默认 false） | `src/utils/config.h` |
| 解析开关 | `src/utils/config.cpp` |
| `evaluate_entry` gate 掉 QR valid 信号 | `src/core/strategy.cpp` |
| 模板加开关 | `config/config.example.json` |
| regime 表 + §2.1 标记退役 | `docs/strategy-btc-1h.md` |
| 加 ADR S6 | `docs/DECISIONS.md` |
| 本报告 | `docs/steps/step-retire-quiet-reversion.md` |
| 保留（惯例） | `src/core/strategy.cpp` 的 `evaluate_cheap_rebound`（休眠，作回放/候选日志） |

## 红线遵守

- [x] 没有引入硬编码密钥
- [x] 没有跳过错误处理（纯 gate 逻辑，候选旁路不变）
- [x] 编译通过，无 warning

## 验收结果

```
cmake --build build -j  → [100%] Built target polymarket-arb（无 warning）
逻辑：abs(dev) ≤ 0.12 仍跑 evaluate_cheap_rebound（候选照常落盘），
      valid 信号在 quiet_reversion_enabled=false 时改判 quiet_reversion_retired，
      主循环 sig.valid=false → 不开仓、写 main_candidates.jsonl。
```

## 部署注意

服务器 `config/config.json` 无 `quiet_reversion_enabled` 字段时按默认 `false` 处理（退役生效），无需改服务器配置即可生效；scp 新二进制 + manager restart 即可。

## 遗留问题

- 频率下降是预期内的副作用；后续若要提频，单独立项改 momentum 入场或死区，不要复活 QR。
- QR 的负 edge 是小样本结论；休眠的 `evaluate_cheap_rebound` + `main_candidates.jsonl` 仍可继续离线积累证据。
