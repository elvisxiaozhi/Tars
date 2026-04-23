# Step 2.10 — 三梯队 Analytics 数据采集 + Dashboard 报表

## 概要

新增 12 个 analytics 字段用于策略复盘，覆盖 MFE/MAE、持仓时长、BTC 出场信息、市场微观结构、时间标签、风控上下文。Dashboard 新增 Analytics 报表区域。

## 新增数据字段

### 第一梯队（策略调参）
| 字段 | 说明 | 采集方式 |
|------|------|----------|
| `max_price` | 持仓期间合约最高 bid (MFE) | 每个 tick 更新 |
| `min_price` | 持仓期间合约最低 bid (MAE) | 每个 tick 更新 |
| `btc_price_at_exit` | 出场时 BTC 价格 | 出场时采集 |
| `btc_deviation_at_exit` | 出场时 BTC 偏移% | 出场时计算 |
| `hold_duration_sec` | 持仓时长（秒） | exit_time - entry_time |

### 第二梯队（规律发现）
| 字段 | 说明 | 采集方式 |
|------|------|----------|
| `spread_at_entry` | 入场时买卖价差 | 开仓时采集 |
| `ask_depth_at_entry` | 入场时 ask 侧总挂单量 | 开仓时遍历 order book |
| `hour_et` | 入场小时 (ET, 0-23) | 开仓时 UTC-5 换算 |
| `day_of_week` | 入场星期 (0=Sun..6=Sat) | 开仓时 UTC 取 |

### 第三梯队（风控复盘）
| 字段 | 说明 | 采集方式 |
|------|------|----------|
| `consec_wins_before` | 入场前连赢次数 | RiskManager 新增 consecutive_wins_ |
| `consec_losses_before` | 入场前连亏次数 | RiskManager 已有 |
| `balance_before` | 入场前账户余额 | 开仓时采集 |

## Dashboard 新增内容

### Trade History 表
- 新增 MFE、MAE、Duration 三列

### Open Positions 表
- 新增 MFE、MAE 两列（实时更新）

### Analytics 报表区域
1. **MFE/MAE 卡片**：平均 MFE、平均 MAE、MFE:MAE 比率、最大 MFE
2. **持仓时长卡片**：平均、最短、最长持仓 + Spread 赢/亏对比
3. **小时热力图**：24 格，每格显示交易次数 + 平均 P&L，绿赢红亏
4. **出场原因分布**：横向进度条，按比例显示各 exit_reason
5. **星期统计**：7 格，Mon-Sun 交易次数 + 平均 P&L
6. **连亏分析**：连亏 2+ 后的平均 P&L vs 正常情况对比

### API 新增
- `/api/analytics` — 返回聚合统计 JSON（MFE/MAE、时长、小时热力图、星期统计、出场分布、spread 分析、连亏分析、资金曲线）

## 文件清单

| 操作 | 文件 | 改动 |
|------|------|------|
| 修改 | `src/core/strategy.h` | Position 增加 10 个 analytics 字段 |
| 修改 | `src/core/risk_manager.h` | 增加 consecutive_wins_ + getter |
| 修改 | `src/core/risk_manager.cpp` | 追踪连赢计数 |
| 修改 | `src/core/trade_journal.h` | TradeRecord 增加 12 个 analytics 字段 |
| 修改 | `src/core/trade_journal.cpp` | JSONL 写入新字段 |
| 修改 | `src/net/api_server.h` | 增加 on_analytics() |
| 修改 | `src/net/api_server.cpp` | 注册 /api/analytics 路由 |
| 修改 | `src/main.cpp` | fill_analytics helper、MFE/MAE tick 追踪、入场采集、/api/analytics 聚合 |
| 修改 | `src/dashboard.h` | Analytics HTML + JS 渲染 |

## 验收结果

```
cmake --build build   # 编译通过，零 warning
```
