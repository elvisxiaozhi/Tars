# Polymarket Crypto 1小时 UP/DOWN 交易策略

> 最后更新：Multi-Crypto Regime v1（Trend 正向 + Reversal 反向 + Quiet Reversion 小仓低波动回归）。依据 `logs/trades.jsonl`
> 全量历史：dry_run 88 个开仓级样本小幅为正但高度依赖少数反向大赢家；live 14 个样本
> 显示强趋势中继续反向买是主要亏损来源。最近低波动 BTC 日志显示，单 BTC 严格 regime 会长时间空仓，
> 因此扩展到 BTC/ETH/SOL/XRP/DOGE/BNB/HYPE 并行监听，但每个币独立参数、统一全局风控。

---

## 一、核心结论

策略不再固定“买便宜的一边”。每个 tick 先判断市场状态：

| 状态 | 动作 |
|------|------|
| Trend Mode | 标的偏离 strike 且偏离继续扩大，买正向 |
| Reversal Mode | 标的极端偏离但开始收缩，买反向便宜票 |
| Quiet Reversion | 低波动、贴近 strike 的小时，小仓位买便宜边 |
| No Trade | 其它情况空仓 |

历史依据：

- live 中 `abs(dev) >= 0.18%` 的反向交易：3 笔，`-$4.60`，0 胜。
- dry 的大额盈利主要来自极端偏离后的反向 TP1/TP2，因此反向不能删除，只能加“衰竭/收缩”条件。
- dry 入场时间分桶中，30-40min left 表现最好；45-50min left 明显较差。

---

## 二、交易标的与参数

默认 dry_run 并行监听：

| 标的 | Binance symbol | Polymarket hourly slug prefix | live 默认 |
|------|----------------|--------------------------------|-----------|
| BTC | `BTCUSDT` | `bitcoin-up-or-down` | enabled |
| ETH | `ETHUSDT` | `ethereum-up-or-down` | disabled |
| SOL | `SOLUSDT` | `solana-up-or-down` | disabled |
| XRP | `XRPUSDT` | `xrp-up-or-down` | disabled |
| DOGE | `DOGEUSDT` | `dogecoin-up-or-down` | disabled |
| BNB | `BNBUSDT` | `bnb-up-or-down` | disabled |
| HYPE | `HYPEUSDT` | `hype-up-or-down` | disabled |

`config/config.json` 可通过 `strategy.crypto_symbols` 和 `coins[]` 覆盖。LIVE 模式会继续监听所有配置币种，
但只有 `live_enabled=true` 的币种允许真下单。

默认参数：

| 标的 | Trend dev / vol | Reversal dev / vol | Quiet dev / vol / entry | Quiet size |
|------|-----------------|--------------------|--------------------------|------------|
| BTC | `0.18% / 0.80` | `0.25% / 0.80` | `0.04-0.12% / 0.60 / <=0.28` | `$1.25` |
| ETH | `0.22% / 0.85` | `0.30% / 0.85` | `0.06-0.16% / 0.65 / <=0.27` | `$1.00` |
| SOL | `0.28% / 0.90` | `0.38% / 0.90` | `0.08-0.22% / 0.70 / <=0.25` | `$0.75` |
| XRP | `0.35% / 0.80` | `0.45% / 0.80` | `0.10-0.25% / 0.60 / <=0.28` | `$0.50` |
| DOGE | `0.40% / 0.80` | `0.50% / 0.80` | `0.12-0.30% / 0.60 / <=0.28` | `$0.50` |
| BNB | `0.25% / 0.80` | `0.35% / 0.80` | `0.07-0.18% / 0.60 / <=0.28` | `$0.50` |
| HYPE | `0.45% / 0.80` | `0.60% / 0.80` | `0.15-0.35% / 0.60 / <=0.28` | `$0.50` |

---

## 三、Regime 判定

每次评估入场时计算：

| 字段 | 含义 |
|------|------|
| `dev` | `(price - strike) / strike * 100` |
| `abs_dev` | `abs(dev)` |
| `expanding` | 最近两个已记录 tick 中 `abs_dev` 连续扩大 |
| `contracting` | 最近两个已记录 tick 中 `abs_dev` 连续收缩 |
| `vol_ratio` | `current_1h_vol / avg_24h_vol` |

新 K 线开始后前两个 tick 只用于建立 slope 历史，不入场。

---

## 四、Trend Mode（正向）

### 入场条件

| 条件 | 规则 |
|------|------|
| 偏离 | `abs_dev >= coin.trend_abs_dev` |
| 斜率 | `expanding == true` |
| 波动 | `vol_ratio >= coin.trend_vol_ratio` |
| 时间 | `35 <= minutes_left <= 45` |
| 价格 | favored side ask 在 `0.50 - 0.75` |

方向：

| 标的状态 | 买入 |
|----------|------|
| `dev > 0` | UP |
| `dev < 0` | DOWN |

仓位：

| 信号 | 投入 |
|------|------|
| BTC 普通/强趋势 | `$2.25 / $2.50` |
| ETH 普通/强趋势 | `$2.25 / $2.50` |
| SOL 普通/强趋势 | `$1.75 / $2.00` |

仓位没有继续压低到 `$1-$1.5`，原因是 live 出场有最小订单约束。Trend 使用分批止盈时，
`$2.25` 可以让 TP 子单大体保持在可成交金额以上。

### Trend 止盈

| 档位 | 触发价 | 卖出比例 |
|------|--------|----------|
| TP0 | `entry + 0.08`，最高 0.90 | 50% |
| TP1 | `entry + 0.15`，最高 0.92 | 剩余全部 |

Trend 票入场价通常较高，不追求从 20¢ 到 90¢ 的大赔率，目标是捕获 8-15¢ 的趋势段。

### Trend 止损

优先级从高到低：

| 类型 | 规则 |
|------|------|
| 硬止损 | `current <= entry - 0.10` |
| BTC 回穿 | UP 仓 `dev <= 0` / DOWN 仓 `dev >= 0` |
| 死水 | 入场 5 分钟后 `MFE < +0.03` |
| trailing 1 | `MFE >= +0.08`：`stop = max(entry + 0.02, peak - 0.06)` |
| trailing 2 | `MFE >= +0.15`：`stop = max(entry + 0.06, peak - 0.05)` |

---

## 五、Reversal Mode（反向）

### 入场条件

| 条件 | 规则 |
|------|------|
| 偏离 | `abs_dev >= coin.reversal_abs_dev` |
| 斜率 | `contracting == true` |
| 波动 | `vol_ratio <= coin.reversal_vol_ratio` |
| 时间 | `30 <= minutes_left <= 38` |
| 价格 | contrarian ask `<= 0.30` |

方向：

| 标的状态 | 买入 |
|----------|------|
| `dev > 0` | DOWN |
| `dev < 0` | UP |

仓位：

| 信号 | 投入 |
|------|------|
| BTC 普通/强回归 | `$2.25 / $2.50` |
| ETH 普通/强回归 | `$2.00 / $2.25` |
| SOL 普通/强回归 | `$1.50 / $1.75` |

### Reversal 止盈

沿用大赔率结构：

| 档位 | 触发价 | 卖出比例 |
|------|--------|----------|
| TP0 | 0.45 | 30% |
| TP1 | 0.70 | 剩余的 `3/7`，即原始约 30% |
| TP2 | 0.90 | 剩余全部 |

### Reversal 止损

优先级从高到低：

| 类型 | 规则 |
|------|------|
| 硬止损 | 从 entry 下跌 `>= 30%` |
| dev 继续扩大 | 当前 dev 与入场 dev 同号，`abs(dev)` 比入场扩大 `> 0.03%`，且价格跌破 `entry - 0.07` |
| 死水 | 入场 8 分钟后 `MFE < +0.02`，且当前亏损不超过 15% |
| trailing 1 | `MFE >= +0.10`：`stop = entry + 0.05` |
| trailing 2 | `MFE >= +0.15`：`stop = max(entry + 0.10, peak - 0.15)` |

---

## 六、Quiet Reversion（低波动回归）

该模式用于处理“BTC 几小时无交易”的低波动场景，但只用小仓位补充频率，不作为主收益来源。

### 入场条件

| 条件 | 规则 |
|------|------|
| 时间 | `25 <= minutes_left <= 35` |
| 偏离 | `coin.quiet_min_dev <= abs_dev <= coin.quiet_max_dev` |
| 波动 | `vol_ratio <= coin.quiet_vol_ratio` |
| 方向 | 买 UP/DOWN 中更便宜的一边 |
| 价格 | cheap side ask `<= coin.quiet_max_entry` |
| 盘口 | `abs(up_ask + down_ask - 1.0) <= coin.max_spread` |

### Quiet 止盈

| 档位 | 触发价 | 卖出比例 |
|------|--------|----------|
| TP0 | 0.42 | 50% |
| TP1 | 0.62 | 剩余全部 |

### Quiet 止损

| 类型 | 规则 |
|------|------|
| 硬止损 | `current <= entry - 0.07` |
| 偏离突破 | `abs_dev > coin.quiet_max_dev + 0.05%` |
| 时间止损 | 剩余 `<=12min` 且还没到 TP0 |

---

## 七、交易频率与风控

- 每个币每根 1h K 线最多 1 次初始开仓。
- 全局每小时最多 2 次初始开仓。
- 全局最多同时 2 个持仓。
- Quiet Reversion 全局每小时最多 2 次，但每个币每根 K 线仍最多 1 次。
- 止损性出场会设置该币 `candle_stopped`，本币本场禁入。
- 剩余时间 `<= 10min` 不新开；已持仓按特殊时间止损处理。
- LIVE 卖出统一经过 dust buffer + server balance retry，避免因为 6 位小数余额误差导致止损卖不出去。
- 为了避免 7 个币互相拖慢：
  - Binance 多币种价格/K 线并发拉取，1h K 线缓存 60 秒。
  - Binance 单币种失败不拖慢整轮；临时错误冷却 30 秒，永久 `status=400` 类错误冷却 5 分钟。
  - Gamma 市场发现失败后对该 slug 冷却 60 秒再重试。
  - Polymarket CLOB 盘口走 WebSocket market channel 并发订阅所有活跃 token。
  - REST `/book` 只做初始/过期兜底；空仓且不在 `25-45min` 入场窗口的市场不刷 book。
  - 已持仓市场如果 WS 盘口过期，只刷新持仓 token 的 order book。
  - LIVE 入场前强制 REST 刷新一次盘口，并重新评估信号；信号变化则跳过。
  - Binance/Gamma/CLOB REST 的扫描 client 使用较短 timeout，单个币种失败只跳过该币/该市场。
  - WS 盘口在 dry_run 超过 10 秒未更新、live 超过 5 秒未更新时，视为 stale 并触发 REST 兜底；兜底失败不新开仓。

---

## 八、费用与执行

| 动作 | 角色 | 费率 |
|------|------|------|
| 入场 | maker | 0% |
| 出场 | taker/FOK | Crypto taker fee：`7.2% * p * (1-p)` |

入场仍以 `ask - 0.01` 挂限价单；出场用 FOK floor price，让 CLOB 按当前可成交 bid 尽快成交。

---

## 九、验证标准

本分支先 dry_run 观察，不直接放大 live。至少收集 30-50 个新开仓级样本后再评估：

| 指标 | 目标 |
|------|------|
| 开仓级胜率 | `> 40%` |
| Profit Factor | `> 1.2` |
| live PnL | 正值 |
| 最大回撤 | 不超过手动可接受范围 |

若 Trend Mode 无法达到正期望，说明正向追随并没有实际优势；若 Reversal Mode 大赢家消失，
说明收缩条件过严或反向赔率不再足够。
