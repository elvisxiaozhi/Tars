# Step 2.2-2.4 — BTC 1h 策略引擎 (Dry Run)

## 概要

实现 BTC 1小时 Up/Down 交易策略的核心模块：Binance 价格数据、策略信号引擎、风控管理、交易日志。以 dry run 模式运行，生成信号但不实际下单。

## 关键命令

```bash
cmake --build build
./build/polymarket-arb              # dry run 模式，Ctrl+C 退出
```

## 文件清单

| 操作 | 文件 |
|------|------|
| 新增 | `src/core/binance_feed.h/cpp` — Binance BTC/USDT 价格 + 1h K线 + 波动率 |
| 新增 | `src/core/strategy.h/cpp` — 入场/止盈/止损/最后10分钟信号逻辑 |
| 新增 | `src/core/risk_manager.h/cpp` — 仓位管理、红线、日回撤 |
| 新增 | `src/core/trade_journal.h/cpp` — 交易记录（JSONL 文件） |
| 新增 | `docs/strategy-btc-1h.md` — 用户策略规则文档 |
| 修改 | `src/main.cpp` — 策略循环主逻辑 |
| 修改 | `src/core/market_feed.h/cpp` — 增量 gamma 拉取 + refresh_order_book |
| 修改 | `src/utils/config.h/cpp` — mode, account_balance, poll_interval_sec |
| 修改 | `config/config.json` — 新增策略参数 |
| 修改 | `CMakeLists.txt` — SOURCES 增加 4 个新 cpp |

## 设计决策

- **Binance K线计算波动率**：用 (high-low)/open 作为 1h 波动率指标，近 24 根 K 线均值作为基准
- **方向确认用偏离率**：BTC 偏离 strike > 0.05% 才视为有方向，避免噪音
- **gamma 增量拉取**：已加载的市场不重复请求，跨小时自动发现新市场
- **止盈4档分仓**：第1档回本卖50%→第2档翻倍卖25%→第3档75¢卖15%→第4档85¢卖10%
- **3层止损**：价格-35%/BTC突破strike 0.3%/时间≤10分钟+价格<20¢
- **费用感知**：计算真实回本价（含手续费 shares × 0.05 × p × (1-p)）
- **信号拒绝可追溯**：每个被拒的信号记录 reject_reason，便于复盘

## 策略规则映射

| 策略文档章节 | 实现位置 |
|-------------|---------|
| §一 入场条件 | `strategy.cpp::evaluate_entry()` |
| §二 限价单 | `main.cpp` 入场价 = ask - 1¢ |
| §三 仓位管理 | `risk_manager.cpp` |
| §四 止盈规则 | `strategy.cpp::compute_tp_levels()` |
| §五 止损规则 | `strategy.cpp::evaluate_exit()` |
| §六 最后10分钟 | `strategy.cpp::evaluate_last_10min()` |
| §七 费率计算 | `main.cpp::calc_fee()` |
| §八 交易记录 | `trade_journal.cpp` → logs/trades.jsonl |
| §九 红线 | `risk_manager.cpp::can_open_position()` |

## 红线遵守

- [x] 不追涨买入超过30¢（strategy.cpp 硬检查）
- [x] 连续亏损3次停止（risk_manager.cpp）
- [x] 日回撤5%停止（risk_manager.cpp）
- [x] 没有硬编码密钥
- [x] API 限流保护
- [x] 编译通过，零 warning

## 验收结果

```
polymarket-arb v0.2.0 [DRY RUN]
Strategy loop: poll every 30s, account=$1000

Tick 1 (16:59, 4AM ET candle 末尾):
  BTC: $75,226 | strike: $75,252 | dev: -0.03% | vol: 0.0030 < avg 0.0044
  → 未入场：波动率不足 + 只剩0分钟

Tick 2 (17:00, 5AM ET candle 开头):
  BTC: $75,257 | strike: $75,257 | dev: +0.00% | vol: 0.0000 < avg 0.0042
  → 自动发现 6AM ET 新市场（增量，只 1 次 gamma 请求）
  → 未入场：波动率0 + 无方向偏离 + 订单簿无合理挂单
```

策略循环正常运行，正确拒绝不满足条件的市场，Ctrl+C 优雅退出并打印摘要。

## 遗留问题

- 当前市场订单簿只有 0.01/0.99 的挂单，没有合理价位（可能因为市场刚创建或流动性不足）
- dry run 模拟成交假设限价单一定能成交，实际需考虑挂单排队
- BTC 止损的"关键支撑/阻力位"目前简化为 strike ± 0.3%，后续可用 ATR 或前期高低点
- 需要在有流动性的时段（美股交易时间 9:30-16:00 ET）验证信号生成
- Step 2.5（EIP-712 签名下单）是 live 模式的前提
