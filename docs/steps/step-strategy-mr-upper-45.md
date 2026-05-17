# Step — 入场时间窗上限 50min → 45min（基于 R2+R3 日志反事实回放）

## 概要

把 main strategy 的入场时间窗口上限从 `minutes_remaining > 50` 收紧到 `>= 45`，剔除最大亏损子集。R2+R3 段（修复后 94 笔）反事实回放显示：净 PnL −1.61 → +2.46（+$4.07），笔数 94 → 65（−31%），wr 50% → 61.5%。

## 关键命令

```bash
# 编译
cmake --build build -j

# dry-run 验证（应看到部分入场被拒，reject_reason=*_time_window 增加）
./build/polymarket-arb 2>&1 | grep -E "SIGNAL|reject_reason"

# 观察 1-2 周后回放新 jsonl，对比 mr 分布与净 PnL
```

## 文件清单

| 操作 | 文件 |
|------|------|
| 修改 | `src/core/strategy.cpp` (L97, L265) |
| 新增 | `prompts/analyze_strategy.md`（本次分析使用的标准化 prompt） |
| 新增 | `docs/steps/step-strategy-mr-upper-45.md`（本报告） |

## 设计决策

### 为什么砍 45-50min 窗口？

R2+R3 段（2026-05-06 ~ 05-13，单仓制 + tighten 之后的"修复后"基线）共 94 笔，按 `entry_price × minutes_remaining` 交叉分桶后，**stop_price 亏损的 92% 集中在 mr ≥ 30** 的入场：

| (mr, entry_price) 桶 | n | stop_price 贡献 |
|---|---|---|
| (30-45min, 0.25-0.40) | 11 | −4.23 |
| (45-60min, 0.15-0.25) | 9 | **−3.76** |
| (45-60min, 0.25-0.40) | 7 | **−2.74** |
| (30-45min, 0.15-0.25) | 2 | −0.79 |
| (30-45min, ≥0.40) | 2 | −0.36 |

砍 `mr >= 45` 一刀切掉两个最大桶之一（共 −6.5 损失），命中精准。

### 为什么不选更激进的 mr < 35？

反事实回放对比（R2+R3 共 94 笔）：

| 过滤规则 | 保留笔数 | 净 PnL | 改善 | 备注 |
|---|---|---|---|---|
| 基线 | 94 | −1.61 | — | |
| **mr < 45**（本次采用） | **65** | **+2.46** | **+4.07** | 保留 69% 频次 |
| mr < 35 | 3 | +0.29 | +1.90 | 样本极小，置信度低 |
| entry_price < 0.20 | 4 | +1.24 | +2.85 | 样本极小 |
| mr<45 AND ep<0.25 | 13 | +1.55 | +3.16 | 双重过滤误杀盈利 |

单一条件 `mr < 45` 在"频次保留率"和"净 PnL 改善"之间取得最优 trade-off。更激进的过滤虽然单笔均盈更高，但样本仅 3-13 笔，无法采信。

### 为什么用 `>= 45` 而非 `> 45`？

回放代码是 `r['minutes_remaining'] < 45`（mr=45 被剔除）。strategy.cpp 改成 `minutes_remaining >= 45 → reject` 完全对应。两个函数（`evaluate_cheap_rebound`、`evaluate_momentum_follow`）同步修改，保持一致。

### 不动的部分

- `evaluate_cheap_rebound` 内 L194 的 `time_41_45` confidence component 保留——新上限 < 45 后该 component 实际只在 mr ∈ [41, 44] 触发，逻辑无副作用，仅命名略不准。后续若评估 confidence 体系再统一处理
- 下限 `mr <= 30`（cheap）和 `mr < 25`（momentum）未动——这部分入场表现较好，无证据需要收紧
- TP/Stop/trailing/dead_water 逻辑全部未动
- 手续费、下单方式（entry maker GTC / exit taker FOK/FAK）未动——属于 P2 优化，待 P0 验证后再考虑

## 红线遵守

- [x] 没有引入硬编码密钥
- [x] 没有跳过错误处理
- [x] 编译通过，无 warning
- [x] 未修改三层账户架构 / V2 字段 / 私钥处理 / 单仓制 / live 启动守卫
- [x] 仅 dry-run 模拟盘逻辑变化，不影响 live 资金流

## 验收结果

```
$ cmake --build build -j
[ 13%] Built target polymarket_crypto
[ 31%] Built target spdlog
[ 36%] Built target import_key
[ 47%] Built target test_crypto
[ 50%] Built target test_eip712
[ 59%] Built target test_chain
[ 61%] Building CXX object CMakeFiles/polymarket-arb.dir/src/core/strategy.cpp.o
[ 63%] Linking CXX executable polymarket-arb
[100%] Built target polymarket-arb
```

## 预期影响

| 指标 | R2+R3 基线 | 改动后预期 | 年化外推 |
|---|---|---|---|
| 笔数 | 94 / 8 天 = 11.8 笔/天 | 65 / 8 天 = 8.1 笔/天 | 2,966 笔/年 |
| 净 PnL | −1.61 | +2.46 | +$112/年（dry-run 规模）|
| wr | 50.0% | 61.5% | — |
| 单笔均盈 | −0.017 | +0.038 | — |
| 频次变化 | — | **−31%** | — |
| 净 PnL 变化 | — | **−1.61 → +2.46（年化 −$73 → +$112）** | — |

## 遗留问题（下一步候选）

按性价比排序的后续优化（**当前 step 完成 + 观察 1-2 周后再评估**）：

1. **回滚或重审 R3 confidence gates**（建议 3）——R3 vs R2 数据显示加 gates 后 gross PnL 从 +2.45 跌到 −0.97。但 `confidence_components` 字段填充率 0%，需要先把字段落盘才能定位是哪些 component 在伤害。预期 +$1.4 / 8 天
2. **maker-TP fallback**（建议 2）——TP partial 出场从 `is_taker=true, FAK` 改为先挂 GTC maker，超时 fallback。预期 +$0.41 / 8 天，但需重构 main.cpp:1769 出场路径
3. **把 `confidence_components / cross_coin_state / entry_price_bucket` 等字段补充落盘到 trades.jsonl**——目前填充率 0%，无法做下一轮优化
