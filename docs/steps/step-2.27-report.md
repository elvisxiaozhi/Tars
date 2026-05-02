# Step 2.27 — Regime strategy: Trend 正向 + Reversal 反向

## 背景

全量历史按开仓级聚合后：

| 模式 | 开仓 | P&L | 胜率 | 结论 |
|------|------|-----|------|------|
| dry_run | 88 | +$10.49 | 30.7% | 小幅正，但依赖少数反向大赢家 |
| live | 14 | -$8.64 | 21.4% | 当前 live 不合格 |

关键发现：

- live `DOWN`：9 笔，0 胜，`-$8.40`。
- live `abs(dev) >= 0.18%` 时继续反向：3 笔，`-$4.60`，0 胜。
- dry 极端偏离反向仍有大赢家，所以不能简单删除反向，必须区分趋势延续与偏离回归。

## 改动

### 1. `StrategyRegime`

新增：

- `StrategyRegime::TREND`
- `StrategyRegime::REVERSAL`

`EntrySignal` 和 `Position` 都带 `regime`，从入场一路传到 TP/SL。

### 2. 入场逻辑

Trend Mode：

- `abs_dev >= 0.18%`
- `abs_dev` 连续扩大
- `vol_ratio >= 0.8`
- `35 <= minutes_left <= 45`
- favored ask `0.50 - 0.75`
- 正向买：`dev > 0` 买 UP，`dev < 0` 买 DOWN

Reversal Mode：

- `abs_dev >= 0.25%`
- `abs_dev` 连续收缩
- `vol_ratio <= 0.8`
- `30 <= minutes_left <= 38`
- contrarian ask `<= 0.30`
- 反向买：`dev > 0` 买 DOWN，`dev < 0` 买 UP

其它全部 `no_regime` 拒绝。

### 3. 仓位

为保证 live TP 子单金额不低于实际可成交范围：

- Trend：`$2.25`，强趋势 `$2.50`
- Reversal：`$2.25`，极端偏离 `$2.50`

### 4. 出场

Trend：

- TP0 `entry + 0.08` 卖 50%
- TP1 `entry + 0.15` 卖剩余
- 硬止损 `entry - 0.10`
- dev 回穿 0 立即退出
- 5min dead water：`MFE < +0.03`
- trailing：`MFE >= +0.08` / `+0.15` 两档

Reversal：

- 保留 `0.45 / 0.70 / 0.90` 三档 TP
- 保留 `-30%` 硬止损
- 新增 dev 继续扩大止损：当前 dev 与入场 dev 同号，且扩大超过 0.03%，价格跌破 `entry - 0.07`
- 保留 Step 2.25 dead_water floor 和 Step 2.20/2.22 trailing

### 5. 每小时最多一次初始开仓

`RiskManager` 增加 `candle_traded_`：

- `add_position()` 后本 K 线不再允许新开仓
- 新 K 线 `reset_candle()` 时清零

## 文件

| 文件 | 说明 |
|------|------|
| `src/core/strategy.h` | 新增 regime 字段和 TP overload |
| `src/core/strategy.cpp` | Regime 入场、两套 TP/SL |
| `src/core/risk_manager.*` | 动态 size 余额检查 + 每小时最多一次开仓 |
| `src/main.cpp` | 使用 `sig.size_usdc/sig.shares`，保存 regime |
| `docs/strategy-btc-1h.md` | 策略文档更新为 Regime v1 |

## 验证

```bash
rtk cmake --build build
```

结果：通过。

## 注意

这是策略实验分支 `strategy-regime-trend-reversal`。先 dry_run 收集 30-50 个新开仓级样本，
再评估是否切 live。目标是验证：

- Trend Mode 是否改善强 dev 时继续反向的问题
- Reversal Mode 是否仍能保留 dry 历史中的大赔率赢家
- `no_regime` 是否有效减少弱信号交易

