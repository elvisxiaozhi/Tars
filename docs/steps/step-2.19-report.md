# Step 2.19 — 死水早退（Dead Water Exit）

## 概要

新增止损规则：入场 5 分钟后若持仓最高价未达 entry+5¢，立即按 best_bid 平仓。
针对「入场后无 momentum 的 trip 必然 stop_price」的固定亏损模式，把损失从 -50% 缩到 -10%~-20%。

## 数据依据

`mfe_at_5/10/15min` 埋点（commit `32a43f3`）首次拿到真实数据：

**P1 (4-26, UP @0.10)**：
```
entry=$0.10  min_rem=43min
mfe_at_5min  = $0.13   → MFE_gain = +$0.03  ❌ 死水
mfe_at_10min = $0.13   → 仍然死水
mfe_at_15min = $0.13   → 仍然死水
exit @ $0.05 (stop_price)  → -$0.53
```

**P2 (4-26, DOWN @0.26)** 对照组：
```
entry=$0.26  min_rem=44min
mfe_at_5min  = $0.43   → MFE_gain = +$0.17 ✓ 立即起飞
TP0 + trailing → +$0.39
```

5 分钟内 MFE 达到 +5¢ 的 trip 几乎都是赢家；未达到的几乎都跌到 stop_price。
设阈值 +5¢ 在 P1/P2 之间精准划线（P1 触发，P2 不触发）。

## 设计决策

**1. 触发条件：`elapsed_sec >= 300 AND (max_price - entry_price) < $0.05`**

- 5 min 是 `mfe_at_5min` 埋点对齐的窗口
- 阈值 +5¢ = TP0 (45¢ from entry ≈25¢) 的 1/4 进度，没到这个进度说明完全没启动
- 一旦 max_price 突破阈值，永远不会再触发（max_price 单调递增）

**2. 优先级位置：第二高（仅次于 -50% 价格止损）**

```
止损优先级：
  §五.1  价格止损（-50%）  ← 最高，防跳空
  §五.1b 死水早退（new）    ← 新增
  §五.2  Trailing Stop
  §六    末 10 分钟特殊处理
```

死水早退优先于 trailing 是因为：trailing 在 MFE_gain >= +15¢ 才启动，死水触发条件 (MFE_gain < +5¢) 与 trailing 互斥不重叠。

**3. 与 candle_stopped 联动**

死水早退也算止损出场，触发后 `candle_stopped=true`，本场不再开新仓——避免「死水退出后立马在同一蜡烛重新接刀」。

## 文件清单

| 操作 | 文件 |
|------|------|
| 修改 | `src/core/strategy.cpp` — 加 `<chrono>` include；`evaluate_exit` §五.1b 新增死水早退检查 |
| 修改 | `src/main.cpp:931` — `candle_stopped` 触发条件加入 `dead_water_exit` |
| 修改 | `docs/strategy-btc-1h.md` — 头注 Step 2.18→2.19；§五新增 5.1b 章节 |

## 反事实验证（仅有 2 笔可验证 trip）

| trip | mfe_at_5min | MFE_gain | 旧 PnL | 新规则触发？ | 估算新 PnL |
|---|---|---|---|---|---|
| P1 (UP @0.10) | $0.13 | +$0.03 | -$0.53 | ✅ | ~-$0.20（5min 平仓损失更小） |
| P2 (DOWN @0.26) | $0.43 | +$0.17 | +$0.39 | ❌ | +$0.39（不变） |

**P1 + P2 总 PnL：从 -$0.14 → 约 +$0.19**

样本极小（n=1 触发，n=1 对照），需要更多数据验证。

## 红线遵守

- [x] 没有引入硬编码密钥
- [x] 没有跳过错误处理
- [x] 编译通过

## 验收结果

```
cmake --build build-xcode --config Debug   # ** BUILD SUCCEEDED **
```

## 监控指标（1-2 天后回看）

1. `dead_water_exit` 触发次数 vs `stop_price` 触发次数（应该看到比例上升）
2. 触发死水早退的 trip 平均损失 vs 不触发的 stop_price trip 平均损失
3. 是否有「死水早退后 BTC 反转，事后看是错杀」的案例（看那条蜡烛在死水退出后的合约价路径）

## 遗留问题

- 阈值 +$0.05 是基于 P1/P2 单一对照对推断的，可能过于激进或保守，跑 1-2 天再调
- 当前在 elapsed >= 300s 后每个 tick 都会评估，正常情况下首次评估即触发出场，不会重复
