# Step 2.8 — P2 + P3：价格止损跳空保护 + TP 卖光自动关闭

## P2：价格止损优先于时间止损（修复 9PM 类异常亏损）

### 问题

`evaluate_exit()` 中 `evaluate_last_10min()` 在 price stop **之前**检查。当进入最后 10 分钟时：
1. 价格已远低于 -50% 止损线（如 0.001）
2. 代码先进入 `evaluate_last_10min` → 触发 time stop
3. price stop 永远没机会执行
4. 结果：exit_reason 为 "stop_time"，出场价 0.001，亏损 -$1.30（正常止损约 -$0.77）

### 修复

将 price stop 检查移到 `evaluate_last_10min` **之前**。无论剩余时间多少，只要价格跌幅 ≥ 50%，立即触发 stop_price。

### 效果

以 9PM 交易为例：入场 0.26，止损价 0.13。即使在最后 10 分钟价格是 0.001：
- 修复前：stop_time @0.001，亏损 -$1.30
- 修复后：stop_price @0.001（或更早在 0.13 触发），亏损 ≤ -$0.77

## P3：TP 卖光后自动关闭仓位（修复 10AM 记录缺失）

### 问题

TP2 设定 sell_pct=1.0 卖光剩余 shares 后，`shares_remaining_pct ≈ 0`，但：
1. `pos.closed` 未设为 true
2. 仓位一直"开着"，占用单仓名额
3. 依赖 expired handler 在市场消失时写最终记录
4. 如果程序在市场消失前重启，仓位丢失，最终记录永远不会写入

### 修复

在 TP 检查循环之后、止损检查之前，新增检测：
- 如果 `shares_remaining_pct < 0.01`（所有 shares 已卖出）
- 自动标记 `pos.closed = true`
- 写入最终累积 TradeRecord（exit_reason = "tp_filled"）
- 释放仓位（`risk.remove_position()`）

### 效果

- TP 全部触发后立即关闭仓位并记录，不依赖 expired handler
- 单仓名额立即释放，可以进入下一笔交易
- 即使程序重启也不会丢失记录

## 文件清单

| 操作 | 文件 |
|------|------|
| 修改 | `src/core/strategy.cpp` — price stop 移到 last_10min 之前 |
| 修改 | `src/main.cpp` — TP 卖光后自动关闭仓位 + 写最终记录 |

## 验收结果

```
cmake --build build   # 编译通过，零 warning
```
