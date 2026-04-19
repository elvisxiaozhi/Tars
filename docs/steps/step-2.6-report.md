# Step 2.6 — 策略重写 + 详细交易记录

## 概要

按用户修改后的策略文档全面重写：6档止盈（费后回本逐级上浮）、止损简化（-50%+时间）、本场止损禁入、每笔买卖详细落盘。

## 止盈规则（6档）

以入场价 20¢ 为例（`breakeven_price` 解二次方程算费后回本价）：

| 档位 | 触发价 | 卖出比例 | 说明 |
|------|--------|----------|------|
| TP1 | ~21.1¢ | 50% | 费后回本 |
| TP2 | ~24.5¢ | 25% | TP1 × 1.10 含费 |
| TP3 | ~31.0¢ | 25% | TP2 × 1.20 含费 |
| TP4 | ~39.3¢ | 25% | TP3 × 1.20 含费 |
| TP5 | 80¢ | 50% | 固定 |
| TP6 | 90¢ | 100% | 全部卖出 |

回本价计算公式：`P - 0.05P(1-P) = E + 0.05E(1-E)`，解二次方程。

## 止损规则

| 类型 | 条件 | 变化 |
|------|------|------|
| 价格止损 | 跌 ≥ 50% | 原 35% → 50% |
| BTC 止损 | 已移除 | — |
| 时间止损 | ≤10min + <20¢ | 不变 |

## 本场止损禁入

- 止损触发后设置 `candle_stopped_ = true`
- 新 K线 通过 market slug 变化检测，自动 `reset_candle()`
- `can_open_position()` 检查 `candle_stopped_`

## 交易记录增强

- 每次 TP 部分卖出单独记录到 `trades.jsonl`（ID 格式: P1-TP1, P1-TP2...）
- 止损清仓也单独记录
- Dashboard 表格增加：ID、Shares、Cost、Revenue、Fee、Reason、P&L
- 所有数据持久化到本地文件

## 文件清单

| 操作 | 文件 |
|------|------|
| 重写 | `src/core/strategy.cpp` — 6档 TP + breakeven 计算 + -50% 止损 |
| 修改 | `src/core/strategy.h` — TakeProfitLevel tier 1-6 |
| 重写 | `src/core/risk_manager.h/cpp` — candle_stopped 机制 |
| 修改 | `src/main.cpp` — candle 检测 + TP 逐笔记录 + 止损设 flag |
| 修改 | `src/dashboard.h` — 交易历史表格 11 列 |
| 用户修改 | `docs/strategy-btc-1h.md` — 完整策略更新 |

## 验收结果

```
cmake --build build   # 编译通过，零 warning
./build/polymarket-arb # 运行正常，时间窗口过滤生效
```
