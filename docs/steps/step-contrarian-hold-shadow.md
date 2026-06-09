# Step — contrarian_hold_v1 shadow（反向·持有到期·低确认验证）

## 概要

新增 shadow 实验 `contrarian_hold_v1`：中段(20-40min) 买**便宜的 underdog**(dev 反方向侧, 0.38-0.48)，不设 TP/止损，持有到 1h 结算(0/1)，近零费。它是已退役 `held_momentum` 的**镜像**——验证"mid-hour move 到收盘是否倾向反转"这条数据里唯一指向 +EV 的线索。**纯 shadow（独立 ExperimentEngine / 持仓 / $20 余额 / jsonl / dashboard tab），零影响 main / trend_v2。**

## 动机 + 诚实定位

held_momentum 21 笔证明:mid-hour favored 侧 hold-to-expiry 只赢 **43% << 入场价 0.60**。同一批市场,**对侧 underdog 就赢 57%、只卖 ~0.40 → EV/股 = 0.57−0.40 ≈ +0.17**。这是全部数据里唯一 +EV 方向。

**但本策略明确是"低确认验证",不是高确认策略:**
- 证据仅 n=21、p≈0.05,且是同一批市场反看(非独立新证据)。
- contrarian = momentum 的镜像 → **在趋势 regime 必然翻负**;现在 +EV 多半只是当前震荡市。
- **绝不能当结论上 live**;目的是前向出独立样本 + 跨 regime 检验。

## 设计

| 维度 | 规则 |
|---|---|
| 名称 / 前缀 | `contrarian_hold_v1` / `R` |
| 币种 | 仅 BTC |
| 时间窗 | mr 20–40（中段） |
| 买哪侧 | **dev 反方向侧（便宜 underdog）**：dev>0→买 DOWN，dev<0→买 UP |
| 入场条件 | \|dev\| ≥ 0.08% · underdog ask ∈ [0.38, 0.48] · 价差 ≤ 2¢ |
| 出场 | **无 TP / 无止损,持有到期结算(0/1)**（empty tp_levels + no-op exit，复用 close_expired） |
| 费用 | 入场 maker≈0 + 到期免费 → 近零费 |
| 仓位 | $1 名义（5 股/$1 下限托底，同真实化口径） |
| 落盘 | `entry_price_bucket`（und_0.38_0.43 / und_0.43_0.48）+ `btc_deviation_pct`（供 ex-post regime 拆分）|

regime 标记走 ex-post：每笔记 entry_time + dev，分析时按 entry 前后 BTC 1h 趋势一致性拆"震荡 vs 趋势"（与 postmortem 重建 regime 同法），不在 bot 内做 live regime 判断（避免引入低确认的 regime 预测器）。

## 文件清单（全部加法）

| 操作 | 文件 | 内容 |
|------|------|------|
| 修改 | `src/core/experiment_engine.h` | 声明 `evaluate_contrarian_hold_v1` |
| 修改 | `src/core/experiment_engine.cpp` | 实现 + 3 dispatch（entry / TP=空 / exit=no-op，并入已有 held 分支） |
| 修改 | `src/main.cpp` | 1 实例 + 3 API 回调 + reset_candle/reset_global_hour/on_market 各 1 行 |
| 修改 | `src/net/api_server.{h,cpp}` | `/api/experiment-contrarian-hold/{status,trades,all-trades}` |
| 修改 | `src/dashboard.h` | "Contrarian Hold" tab + 面板 + fetch + render（复用 renderExperiment）|
| 修改 | `docs/strategy-experiments.md` | 4→5 活跃 + 描述（含低确认/regime 警示）|

## 设计决策

- **价带 0.38-0.48 = held_momentum favorite 带(0.52-0.62)的镜像**：直接对照那 21 笔的 +EV 线索。
- **窗口 20-40 与 held_momentum 同**：同区对照,排除时间窗混淆。
- **不加 live regime 门**：regime 预测是低确认,会污染验证;改为落 dev + ts 供 ex-post 拆分。
- **判据先定死**：分桶 underdog 真实胜率 > 入场价 + net 正 + ≥80-100 仓 + **跨 regime(含趋势段)仍正**;趋势段放血即证伪。

## 红线遵守

- [x] 没有引入硬编码密钥
- [x] 没有跳过错误处理
- [x] 编译通过、链接成功（api_server 既有 bugprone lint 与本步无关）
- [x] main / trend_v2 / finance / crypto_4h / crypto_daily 零改动

## 验收结果

```
cmake --build build -j → [100%] Built target polymarket-arb
strings 二进制含 contrarian_hold_v1 / experiment-contrarian-hold / contra_price_band
dashboard tab: ...+ Contrarian Hold；KAT 全过
```

## 后续 / 成功判据

1. 攒 ~80-100 仓后:按 `entry_price_bucket` 看 underdog 真实到期胜率 vs 入场价。
2. **必须跨一次 regime 翻面检验**:若只在震荡段 +、趋势段 − → 它只是反动量的 regime 赌 → 退役。
3. 若跨 regime 仍稳定 net 正 → 才提示可能存在结构性 favorite 高估(favorite-longshot bias),届时另开 step + 风控复核再谈 live。
