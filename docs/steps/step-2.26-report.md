# Step 2.26 — live sell dust buffer + balance retry

## 概要

2026-05-02 LIVE 止损时，策略已触发 `stop_price`，但 SELL FOK 连续被 CLOB 拒绝：

```text
SELL shares=10.0000
HTTP 400 ... balance: 9996900, order amount: 10000000
```

Polymarket 以 6 位小数记录 outcome token：

| 字段 | shares |
|------|--------|
| server balance `9996900` | 9.996900 |
| order amount `10000000` | 10.000000 |

本地仓位按理论 10 shares 卖出，但服务器可用余额只有 9.996900，导致止损卖单无限失败。

## 改动

1. `LiveTrader::place_exit_order` 统一处理 LIVE SELL 数量：
   - 请求数量先扣 `0.005 shares` dust buffer
   - 向下取 6 位小数，匹配 CLOB balance 精度
2. 如果 SELL 仍失败且错误中包含 `balance: N`：
   - 解析服务器返回的真实可用 balance
   - 按该 balance 再扣 buffer
   - 同一 tick 立即重试一次
3. 主循环记账改为只要返回 `filled_shares > 0` 就使用真实卖出数量；成交均价缺失时继续用策略价格兜底。

## 覆盖范围

所有 LIVE 出场都经过 `place_exit_order`，因此自动覆盖：

- TP0 / TP1 / TP2
- `stop_price`
- `trailing_stop`
- `dead_water_exit`
- `stop_time`
- emergency close

## 设计取舍

优点：

- 避免因 `10.000000 > 9.996900` 这种 dust 误差卡死止损
- 失败后不等下一 tick，立即按服务器真实 balance 降量重试
- 不改策略判断，只加执行层保护

代价：

- 可能留下极小 dust 仓位，例如 `0.001 ~ 0.005 shares`
- TP 三档后可能不会数学上精确归零，后续需要用真实剩余 shares 替代百分比模型进一步优化

这个 trade-off 可接受：dust 价值通常小于千分之一美元，而止损卡死可能导致数十美分到数美元级别损失。

## 验证

```bash
rtk cmake --build build
rtk git diff --check
```

结果：均通过。

