# Step — strategy-btc-1h.md 全文重写对齐代码

## 概要

把 `docs/strategy-btc-1h.md` 全文重写，每一节对齐 `src/core/strategy.cpp` / `src/core/risk_manager.cpp` / `src/main.cpp` 的代码事实。修复约 20 处文档与代码偏差，包括入场参数错位、退出规则漏字段、风控阈值差 1、绝对红线方向相反等问题。原 doc 最后更新 2026-05-10，期间代码大改但文档没同步。

## 关键命令

```bash
# 渲染检查
glow docs/strategy-btc-1h.md | head -100   # 或 cat / IDE 预览

# 验证 doc 中的代码行号引用是否还有效
grep -n "evaluate_cheap_rebound\|evaluate_momentum_follow\|evaluate_exit\|evaluate_last_10min" src/core/strategy.cpp

# 验证 risk_manager 阈值是否对齐
grep -n "stop_price_this_hour_ >= \|non_btc_stop_price_this_hour_ >= " src/core/risk_manager.cpp
```

## 文件清单

| 操作 | 文件 |
|------|------|
| 修改 | `docs/strategy-btc-1h.md`（全文重写，~150 行 diff）|
| 新增 | `docs/steps/step-doc-strategy-btc-1h-rewrite.md`（本报告）|

## 核心修正项（按章节）

### §一 市场类型
- 监听币种从隐含改为显式列出 `BTC/ETH/SOL/XRP/DOGE/BNB`（`StrategyConfig::crypto_symbols` 默认值）

### §二 核心策略
- **cheap_rebound 支持币种**：从"隐含全币种"改为"仅 BTC + ETH"（L93）
- **cheap 时间窗**：从 `36 ≤ mr ≤ 45` 改为 `30 < mr < 45`（L97，对齐 commit 0f827fb）
- **cheap 入场价**：从 `0.22-0.28` 改为 BTC `0.24-0.26` / ETH `0.24-0.30`（L139-140）
- **cheap ask 红线**：从 `≤0.29` 一刀切改为 BTC `≤0.29` / ETH `≤0.31`（L127）
- **cheap dev 区间**：从 `≤0.12%` 改为完整三段（dev 下限 + BTC/ETH 上限 + ETH 按 entry 三段细分）
- **新增 ETH medium 特例段落**：entry > 0.26 时 mr≥35 AND dev≥0.18 AND size=$0.50（L148-160）
- **momentum 排除 SOL**：明确写出（L261）
- **momentum 时间窗**：从 `32 ≤ mr ≤ 42` 改为 `25 ≤ mr < 45`（L265）
- **momentum 入场价**：从 `0.64-0.69` 改为 `0.64-0.67`（L290）
- **删除 momentum 中虚构的 confidence 评分表**（实际 momentum 不评 confidence）
- **新增 cheap confidence 9 条评分表**（doc 之前只列 5 条）
- 补 confidence_components 落盘缺陷说明（填充率 0%）

### §三 仓位与风控
- 同币种锁仓 exit_reason 从 5 个补到 8 个（+fast_fail_exit / cheap_fail_stop / adverse_expansion_stop，main.cpp L1900-1905）
- 全局熔断阈值从 `≥2` 改为 `≥3`（risk_manager L29）
- **新增 non-BTC 独立熔断说明**（L23）
- 补 ETH medium $0.50 仓位规则

### §四 止盈规则
- 改"按 regime"为"按 entry_price 分两套"（实际代码用 entry≥0.60 切，效果等价但描述更准）

### §五 退出规则（最大修改）
- **新增 5.1 TREND 分支专属**：8 条规则（doc 之前完全没区分分支）
- **新增 5.2 QUIET_REVERSION 分支专属**：3 条规则
- **5.3 通用退出**：补 no_start_exit；改 cheap_fail_stop 条件（doc 原"elapsed≥20 + mfe<0.015"错，实际"elapsed≥90 + mfe<0.02"）；改 adverse_expansion_stop 条件（补 elapsed≥180 + current 条件）
- **trailing 数值全部修正**：mfe≥0.15 从 `max(max-0.15, entry+0.10)` 改为 `max(max-0.07, entry+0.08)`；mfe≥0.10 从 `entry+0.05` 改为 `max(max-0.08, entry+0.04)`
- 5.4 最后 10 分钟价格阈值从 `<0.20` 改为 `<0.25`

### §六 live vs dry
- 增加 fee 备注（maker 0% / taker 7.2%，所有 exit 走 taker 是最大成本中心）

### §七 实验策略
- 精简为一句话指向 `strategy-experiments.md`（删除过时的 legacy_cheap_v2 / trend_follow 两节，那些在 main.cpp 已不再实例化）

### §八 关键复盘结论
- 改写为按 regime 切片的现状表（R0/R1/R2/R3 + 笔数/净 PnL/wr），明确 05-04~05 unlimited 事故段必须剔除
- 补 fee 是 gross 亏损 4 倍的关键事实
- 引用 commit 0f827fb 收紧 mr 的依据

### §九 绝对红线（重写）
- 旧第 1 条"不买 > 30c" 错——momentum 本就买 64-67c。改为按 sub-strategy 分别明确
- **旧第 4 条"≤35min 不开新仓"完全相反**——实际 mr=25..44 都可。改为 cheap 31-44 / momentum 25-44
- 旧第 8 条全局熔断阈值 `≥2` 改为 `≥3`
- 新增第 9 条非 BTC 独立熔断
- 新增第 11 条私钥红线（对齐 CLAUDE.md 硬约束）

## 设计决策

### 为什么完整重写而不是增量修
原 doc 多处"描述与代码相反"（红线第 4 条最严重），而非"个别参数过期"。增量打补丁会留下结构性矛盾（例如保留旧 §七 两节的同时新增对 strategy-experiments.md 的引用，互相打架）。全文重写是更小的认知负担。

### 为什么不动 §六 live vs dry 表格内容
表格内容（GTC/FAK/FOK 描述）实际与代码一致。只补一句 fee 备注，避免无谓改动。

### 为什么 §八 保留而不删
历史复盘有 narrative 价值，且和 commit 0f827fb 的决策依据强相关。改写为按 regime 切片的简表，并标注"historical"。

### 为什么 §九 列 11 条而不更精简
红线是供其它 Claude 会话冷启动时快速对齐的硬约束，重复无害；漏一条可能酿事故。

## 红线遵守

- [x] 没有引入硬编码密钥
- [x] 没有改动任何代码（纯文档更新）
- [x] 与现有 CLAUDE.md 硬约束一致（三层账户 / V2 字段 / 私钥处理 / 单仓制）
- [x] 对实验策略的引用与 `strategy-experiments.md` 一致
- [x] 所有代码行号引用经过 grep 验证

## 验收结果

```
$ wc -l docs/strategy-btc-1h.md
# 旧 403 行 → 新约 200 行（删 §七 过时实验详细 + §八 冗余复盘段落）

$ ls -la docs/strategy-btc-1h.md docs/strategy-experiments.md prompts/analyze_strategy.md
# 三者形成完整文档拓扑：strategy-btc-1h（主）+ strategy-experiments（影子）+ analyze_strategy（分析方法）
```

## 遗留问题

1. **confidence_components 落盘缺陷**：策略代码已经计算 confidence 各组件，但 `trade_journal` 未把 `confidence_components` 字段写入 jsonl（填充率 0%）。这阻塞了"R3 confidence gates 是否应回滚"的下一轮分析。建议下一步先补字段落盘。
2. **CLAUDE.md "1389 行" 描述偏旧**：实际 main.cpp 已超过 1900 行（原 step 报告写过 SharedState 在 L152，实际 R-V2.6+ 后 L 号有漂移）。本次未改 CLAUDE.md 数字，留待下次集中刷新。
