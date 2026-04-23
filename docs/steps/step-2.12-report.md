# Step 2.12 — 扩展 Analytics 指标体系 + Dashboard 展示

## 概要

新增三梯队共 15+ 个分析指标，覆盖盈利效率、方向统计、出场效率、风险评估、分桶对比。所有指标均通过 `/api/analytics` 返回，前端 Dashboard 完整展示。每笔交易新增 `mfe_capture_rate` 字段写入 JSONL，便于策略停止后离线分析。

## 新增指标

### 第一梯队 — 调参决策

| 指标 | 说明 | 前端展示 |
|------|------|----------|
| Profit Factor | 总赢利 / 总亏损 | 卡片，>1 绿色 |
| EV per Trade | 平均每笔蜡烛预期收益 | 卡片 |
| Avg Win / Avg Loss | 平均赢亏额 + 比率 | 卡片，双色 |
| Max Drawdown | 资金曲线峰谷差 | 卡片，红色 |
| MFE Capture Rate | 出场效率 = (exit-entry)/(max-entry) | 3 个卡片：全部/赢/亏 |
| TP Hit Rates | TP0/TP1/TP2/trailing_stop 触发次数 vs 蜡烛数 | 进度条 |
| Direction Analysis | UP vs DOWN 胜率、平均 P&L、总 P&L（蜡烛级） | 双卡片 |
| Equity Curve | 资金曲线折线图 | SVG 图表 |

### 第二梯队 — 风险评估

| 指标 | 说明 | 前端展示 |
|------|------|----------|
| Entry Price Buckets | 0-15¢/15-20¢/20-25¢/25-30¢ 分桶：交易数、胜率、平均 P&L | 列表 |
| BTC Deviation Buckets | <-0.3%/-0.3~0%/0~+0.3%/>+0.3% 分桶 | 列表 |
| Trailing Stop Stats | trailing_stop 出场次数、平均 P&L vs price_stop | 对比卡片 |

### 第三梯队 — 辅助观察

| 指标 | 说明 | 前端展示 |
|------|------|----------|
| Duration Buckets | 0-15m/15-30m/30-45m/45-60m 分桶 | 列表 |
| Win/Loss Ratio | avg_win / abs(avg_loss) | 包含在 Avg Win/Loss 卡片中 |

## 每笔交易新增字段（JSONL 持久化）

| 字段 | 说明 |
|------|------|
| `mfe_capture_rate` | (exit_price - entry_price) / (max_price - entry_price)，出场效率 |

其余聚合指标均可从现有 JSONL 字段离线计算，无需额外存储。

## 文件清单

| 操作 | 文件 | 改动 |
|------|------|------|
| 修改 | `src/core/trade_journal.h` | TradeRecord 增加 `mfe_capture_rate` |
| 修改 | `src/core/trade_journal.cpp` | JSONL 写入 `mfe_capture_rate` |
| 修改 | `src/main.cpp` | fill_analytics 计算 mfe_capture_rate；/api/analytics 新增 15+ 聚合指标；/api/trades 输出 mfe_capture_rate |
| 修改 | `src/dashboard.h` | 新增 6 组展示区域：核心指标卡片、MFE 捕获率卡片、方向分析、TP 触发率、资金曲线 SVG、分桶分析×3、Trailing Stop 对比 |

## 验收结果

```
cmake --build build   # 编译通过，零 warning
```
