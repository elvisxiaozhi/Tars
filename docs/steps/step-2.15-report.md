# Step 2.15 — 接入 Polymarket 真实费率模型

## 概要

把硬编码的 `shares × 0.05 × p × (1-p)` 双边收费模型，替换成 Polymarket 官方真实费率：
maker 0%、Crypto category taker 7.2%、公式形态 `shares × rate × p × (1-p)`，
并按入场（limit 挂单 = maker）/ 出场（best_bid 吃单 = taker）正确分边。

## 数据来源

- [docs.polymarket.com/trading/fees](https://docs.polymarket.com/trading/fees)
  - Maker fee: 0%（永不收费）
  - Crypto taker rate: 7.2%
  - 公式：`fee = C × feeRate × p × (1 − p)`
- Polygon gas: 平均 $0.001-$0.01/笔，本次以 0 模型化（留 config 字段备用）

## 设计决策

**1. 入场=maker，出场=taker，按动作语义分边**

| 路径 | maker/taker | 理由 |
|---|---|---|
| 入场（`sig.entry_price = ask - 1¢`） | maker | 限价单挂在 ask 下方等成交，是 add liquidity |
| TP / Trailing / Stop / StopTime 出场 | taker | 用 `best_bid` 价直接吃买单，是 remove liquidity |
| Expired（市场消失自动结算） | maker | 到期自动结算，不是吃单动作，按 0 费 |

**2. `entry_fee_portion` 保留但恒为 0**

代码路径 `pnl = sell_value - cost_basis - exit_fee - entry_fee_portion`
不动结构，只是参数 `is_taker=false` 自然让 `entry_fee_portion = 0`。
留着是为日后 maker_fee 不再为 0 时（比如 Polymarket 改政策）一行配置就生效。

**3. Fee config 化**

新增 `FeeConfig`，三个字段（maker / taker / gas）全部可在 `config.json` 覆盖，
代码无需改动即可适应费率调整或迁移到其他 category（Sports 3% / Politics 1% 等）。

## 文件清单

| 操作 | 文件 |
|------|------|
| 修改 | `src/utils/config.h` — 新增 `FeeConfig` struct |
| 修改 | `src/utils/config.cpp` — 解析 `fees` 块 |
| 修改 | `src/main.cpp` — `calc_fee` 改签名；7 个调用点显式传 maker/taker |
| 修改 | `config/config.json` + `config/config.example.json` — 新增 `fees` 块 |

## 预期影响

- 入场费 5% → 0%，每笔节省约 $0.05-$0.15
- 出场 taker rate 5% → 7.2%，但仅对 50¢ 附近 TP 出场显著（深价位 p×(1-p) 很小）
- 整体 PnL 略上调（约 30%）
- 旧记录不重算（用户决策）；分析时按 commit `32a43f3` 之后为新模型

## 红线遵守

- [x] 没有引入硬编码密钥
- [x] 没有跳过错误处理
- [x] 编译通过

## 验收结果

```
cmake --build build-xcode --config Debug   # ** BUILD SUCCEEDED **
```

## 遗留问题

- Polymarket 若未来改 protocol fee，只需改 `config.json` 数值
- 若使用其他 category 市场（非 Crypto），需要扩展 `FeeConfig` 支持按 category 分别配置
- maker rebate 程序未建模（当前 Polymarket 仅 registered MM 可领，普通用户拿不到）
