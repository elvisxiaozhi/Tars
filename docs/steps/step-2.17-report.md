# Step 2.17 — 仓位 17 → 10 shares（适配 Polymarket 真实最小订单约束 + 整数股切分）

## 概要

把固定仓位从 17 shares 降到 10 shares，单笔投入下降 41%。
TP 切分天然落到整数股 3/3/4，避开 Polymarket 整数股精度风险。
配合 Step 2.15 接入的真实费率模型（maker 0% / Crypto taker 7.2%），出场用市价单覆盖 $1 USDC 最小订单约束。

## 数据依据

Polymarket 最小订单约束（[help.polymarket.com](https://help.polymarket.com/en/articles/13364481-does-polymarket-have-trading-limits)）：
- **Limit order**: 5 shares 最小
- **Market order**: $1 USDC 最小

## 设计决策

**1. 为什么是 10 shares 而不是更小**

| N | TP 切分（30/30/40） | 整数？ | 评 |
|---|---|---|---|
| 17（旧） | 5.1 / 5.1 / 6.8 | ❌ | 每档 ≥ 5 share，limit 出场也 OK |
| **10** | **3 / 3 / 4** | ✅ 完美整数 | **采用** |
| 8 | 2.4 / 2.4 / 3.2 | ❌ 依赖小数股 API 精度 | 风险 |
| 5 | 1.5 / 1.5 / 2 | ❌ 砍 TP 结构 | 不采用 |

10 shares 的关键优势：30/30/40 切分天然落整数，不依赖 Polymarket 是否支持小数股。8 shares 会切出 2.4 share，若 API 截整数就退化成 2/2/4（25/25/50）。

**2. 出场必须用市价单（market order）**

| 出场场景 | 最小订单覆盖 |
|---|---|
| TP0（3 × $0.45 = $1.35） | ✓ |
| TP1（3 × $0.70 = $2.10） | ✓ |
| TP2（4 × $0.90 = $3.60） | ✓ |
| Stop（10 × $0.10 = $1.00） | 临界 ⚠️ |
| 入场（limit ≥ 5 shares） | ✓ |

低价合约（entry < $0.15）止损时可能触碰 $1 市价单门槛——遗留问题，live 准备时加 fallback 到 limit-at-bid。

## 文件清单

| 操作 | 文件 |
|------|------|
| 修改 | `src/core/risk_manager.cpp` — `fixed_shares_(17.0)` → `fixed_shares_(10.0)`；2 处注释更新 |
| 修改 | `docs/strategy-btc-1h.md` — 4 处同步：版本注、挂单方式、仓位、TP 表、§六手续费整段重写 |
| 修改 | `docs/design.md` — §10 taker 费率描述、§费用结构按 category 区分 |

## 预期影响

| 指标 | 17 shares | 10 shares | 变化 |
|---|---|---|---|
| 单笔投入（@ $0.22） | $3.74 | $2.20 | ↓41% |
| 最大止损损失 | ~$1.87 | ~$1.10 | ↓41% |
| 占 $20 账户 | 19% | 11% | ↓ |
| 大赢上限（4PM UP 类） | $9.14 | ~$5.40 | ×0.59 |
| 并发仓位预算 | ~5 笔 | ~9 笔 | ↑80% |
| 胜率 / R / 期望 | 不变 | 不变 | — |

## 红线遵守

- [x] 没有引入硬编码密钥
- [x] 没有跳过错误处理
- [x] 编译通过

## 验收结果

```
cmake --build build-xcode --config Debug   # ** BUILD SUCCEEDED **
```

## 遗留问题

- **Live 准备前必须**：单笔验证 Polymarket 是否接受小数股（虽然 10 share 切分是整数，但若未来调整 N 仍需此知识）
- **Live 准备前必须**：出场代码加 `shares × price < $1` 校验，触发时 fallback 到 limit-at-bid（≥ 5 share 满足）
- 当前 dry_run 模拟器不区分 market/limit 订单类型，行为等价
