# Step 2.5b-d — 策略简化 + 订单簿修复 + Dashboard 增强

## 概要

三次迭代优化：修复核心订单簿 bug，简化入场策略，调整仓位管理，增强 Dashboard 显示。

## Step 2.5b — 修复订单簿排序 bug + 简化入场策略 (commit 577d2c3)

### 问题
CLOB API `/book` 返回的 bids/asks **未按价格排序**。代码取 `front()` 导致拿到最差价格（bid=0.01, ask=0.99），实际 best bid=0.49, best ask=0.50。

### 修复
`json_helpers.h::extract_best_bid_ask()` 改为遍历找最高 bid 和最低 ask。

### 策略简化
- 移除方向偏离条件（0.05% deviation），改为选择更便宜的一方买入
- 价格上限统一为 ask < 30¢（不再按剩余时间分档）
- tick 日志增加 Up/Down ask 价格显示

| 操作 | 文件 |
|------|------|
| 修改 | `src/utils/json_helpers.h` — 遍历找极值 |
| 修改 | `src/core/strategy.cpp` — 去掉方向偏离、统一价格上限 |
| 修改 | `src/main.cpp` — tick 日志打印 Up/Down ask |
| 修改 | `config/config.json` — poll_interval 30s→10s |

---

## Step 2.5c — 单仓制 + 固定 5 shares (commit 475decb)

### 改动
- **单仓制**：同一时间最多持有 1 个仓位，未全部卖出前禁止开新仓
- **固定下单量**：每次 5 shares（不再按百分比计算）
- **账户余额**：1000U → 10U
- **轮询间隔**：10s → 5s

| 操作 | 文件 |
|------|------|
| 修改 | `src/core/risk_manager.h` — 去掉 position_pct/max_position_pct，改为 fixed_shares |
| 修改 | `src/core/risk_manager.cpp` — max_concurrent 2→1，固定 5 shares |
| 修改 | `src/main.cpp` — shares 不再由 USDC/price 计算 |
| 修改 | `config/config.json` — account_balance=10, poll=5s |
| 修改 | `docs/strategy-btc-1h.md` — §三仓位管理、§九红线更新 |

---

## Step 2.5d — Dashboard 增强 + 只查当前小时市场 (commit 2403f1b)

### Dashboard 改动
4 卡片布局：

| 卡片 | 内容 |
|------|------|
| Account Balance | 总金额，BTC 价格 + 偏离率 |
| Position Cost | 持仓买入成本，未实现盈亏 |
| Realized P&L | 已实现总盈亏，日盈亏 |
| Win Rate | 胜率，胜/败数 |

### 市场查询优化
- 只查询当前小时市场（去掉 offset=1 的下一个小时）
- 每次 fetch 清理不属于当前 slug 的旧市场

| 操作 | 文件 |
|------|------|
| 修改 | `src/dashboard.h` — 4列布局，新增 balance/cost/unrealized 卡片 |
| 修改 | `src/main.cpp` — API 返回 account_balance/total_cost/unrealized_pnl |
| 修改 | `src/core/market_feed.cpp` — 只查当前小时，清理过期市场 |

---

## 当前策略总结

**入场**：剩余 > 30min，ask < 30¢，买更便宜的一方，挂 ask-1¢ 限价单
**仓位**：固定 5 shares，单仓制
**止盈**：4档（1.02x 50%、2x 25%、75¢ 15%、85¢ 10%）
**止损**：价格-35%、BTC偏离0.3%、时间≤10min+价格<20¢
**风控**：连亏3次/日亏5% 熔断
