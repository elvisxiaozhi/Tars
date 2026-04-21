# Step 2.9 — 六项优化：P&L 修正 + 动态余额 + K线级胜率 + 10 shares

## 改动总结

### 1. K 线级 Win Rate（trade_journal.cpp）

**问题**：之前按每条记录算 win/loss，TP 子记录和最终平仓记录各算一次，同一仓位被计算多次。

**修复**：新增 `candle_count()` / `candle_wins()` / `candle_losses()` / `candle_win_rate()`，按仓位 base ID 分组（去掉 "-TP1"/"-TP2" 后缀），汇总同一仓位所有记录的 P&L，正则判赢负。

### 2. 单笔 P&L 修正（main.cpp）

**问题 A — 双重计算**：TP 子记录各记自己的 pnl，但最终平仓记录用的是 `pos.realized_pnl`（累积值）。`total_pnl()` 对所有记录求和 → TP 利润被重复计算。之前显示 +$13.66 的真实利润约为 +$1.67。

**修复**：所有记录（TP / stop / expired）只记录**本次卖出**的 pnl，不再写累积值。

**问题 B — 缺少入场手续费**：P&L 计算只扣了卖出手续费，漏了买入手续费。每笔约 $0.05。

**修复**：每次卖出按比例扣除入场手续费：
```
entry_fee_portion = calc_fee(sell_shares, entry_price)
pnl = sell_value - cost_basis - exit_fee - entry_fee_portion
```

### 3. 动态 Account Balance（risk_manager + main.cpp）

**问题**：Dashboard 显示固定的配置值，不随交易变化。

**修复**：
- 开仓时：`balance -= (shares × entry_price + entry_fee)`
- 卖出时：`balance += (sell_shares × sell_price - exit_fee)`
- 开仓前检查余额是否充足
- API 返回 `risk.account_balance()` 替代 `cfg.strategy.account_balance`

### 4. 交易记录增加 BTC 关键信息（trade_journal.cpp）

**问题**：`avg_vol` 字段一直写 0。

**修复**：Position 新增 `avg_vol` 字段，开仓时从 `btc.avg_24h_vol` 赋值，所有 TradeRecord 正确传递。JSONL 已有 btc_price、btc_strike、btc_deviation_pct、entry_vol、avg_vol 五个字段。

### 5. P&L 验证

修正后的 P&L 计算公式（以一笔止损为例）：
```
入场：10 shares @ 0.28
  cost_basis = 10 × 0.28 = $2.80
  entry_fee  = 10 × 0.05 × 0.28 × 0.72 = $0.1008

止损 @ 0.14：
  sell_value = 10 × 0.14 = $1.40
  exit_fee   = 10 × 0.05 × 0.14 × 0.86 = $0.0602
  pnl = 1.40 - 2.80 - 0.0602 - 0.1008 = -$1.5610
```

修正后的止盈到期场景：
```
入场：10 shares @ 0.28
  cost_basis = 10 × 0.28 = $2.80
  entry_fee  = $0.1008

TP1 @ 0.80, 卖出 5 shares：
  sell = 5 × 0.80 = $4.00
  exit_fee = 5 × 0.05 × 0.80 × 0.20 = $0.0400
  entry_fee_portion = 5 × 0.05 × 0.28 × 0.72 = $0.0504
  pnl = 4.00 - 1.40 - 0.04 - 0.0504 = +$2.5096

TP2 @ 0.90, 卖出 5 shares：
  sell = 5 × 0.90 = $4.50
  exit_fee = 5 × 0.05 × 0.90 × 0.10 = $0.0225
  entry_fee_portion = $0.0504
  pnl = 4.50 - 1.40 - 0.0225 - 0.0504 = +$3.0271

总 P&L = 2.5096 + 3.0271 = +$5.5367
```

### 6. 10 Shares（risk_manager.cpp）

从 5 shares 改为 10 shares。手续费公式 `shares × 0.05 × p × (1-p)` 是线性的，不省手续费，但绝对盈亏翻倍。初始余额从 $10 → $20 以适配。

## TP 卖光后处理（P3 优化）

TP 全部触发后（shares_remaining < 1%），直接关闭仓位释放单仓名额，不再写多余的 journal 记录。TP 子记录已完整覆盖所有卖出。

## 文件清单

| 操作 | 文件 | 改动 |
|------|------|------|
| 修改 | `src/core/strategy.h` | Position 增加 avg_vol、entry_fee 字段 |
| 修改 | `src/core/risk_manager.h` | 增加动态余额方法 |
| 修改 | `src/core/risk_manager.cpp` | 10 shares、余额检查、初始化 |
| 修改 | `src/core/trade_journal.h` | K 线级 candle_wins/losses/count/win_rate |
| 修改 | `src/core/trade_journal.cpp` | 按仓位 ID 分组统计、去除旧 wins/losses |
| 修改 | `src/main.cpp` | P&L 修正、动态余额、avg_vol 传递 |
| 修改 | `config/config.json` | account_balance 10 → 20 |

## 验收结果

```
cmake --build build   # 编译通过，零 warning
```

## 重要提醒

旧的 `logs/trades.jsonl` 数据格式不兼容（P&L 含双重计算），建议备份后清空重跑：
```bash
mv logs/trades.jsonl logs/trades.jsonl.bak2
```
