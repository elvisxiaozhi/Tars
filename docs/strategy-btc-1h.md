# Polymarket 1小时 UP/DOWN 交易策略

> 最后更新：2026-05-07。当前主策略是多币种监听版 `legacy_cheap`：dry run 可模拟多币种，live 模式只允许 BTC 真实交易。

---

## 一、市场类型

- **标的**：Polymarket Crypto 1小时 UP/DOWN 二元期权
- **监听币种**：BTC、ETH、SOL、XRP、DOGE、BNB
- **live 交易币种**：只允许 BTC
- **dry run 交易币种**：允许多币种
- **合约价格区间**：$0 - $1
- **到期结算**：胜方结算 $1，败方结算 $0

---

## 二、核心策略：legacy_cheap

`legacy_cheap` 的核心思想仍然是：在 1 小时 UP/DOWN 市场中，寻找价格较低的一边，用小额仓位买入，等待反弹或结算方向确认。

但当前版本不再是“低于 30c 就买”。必须同时满足盘口质量、价格区间和剩余时间要求。

### 入场条件

以下条件必须全部满足：

| 条件 | 当前规则 |
|------|----------|
| 时间窗口 | `minutes_remaining > 35` |
| 方向选择 | 买入 UP / DOWN 中 ask 更低的一方 |
| ask 红线 | `candidate_ask <= 0.30` |
| 入场价区间 | 按币种配置：BTC/SOL `0.18-0.26`，ETH `0.20-0.26`，其它 `0.20-0.24` |
| spread 过滤 | 按币种配置：BTC/SOL `<= 0.03`，其它更严；`0.20 <= entry < 0.25` 时必须 `<= 0.01` |
| dev 过滤 | `abs(dev)` 不超过币种阈值；且不能明显偏向候选方向的反面 |
| 挂单价格 | `entry_price = candidate_ask - 0.01`，最低 0.01 |
| 单笔成本 | `size_usdc = $1.00` |
| 同币种持仓 | 同一币种已有未平仓仓位时，不再开该币种 |
| live 限制 | live 模式只交易 BTC |

### spread 过滤说明

`spread = ask - bid`。

如果 ask 很低但 bid 更低，买入后马上按 bid 估值会出现很大的天然浮亏。昨晚日志显示：

- `spread >= 3c` 的交易合计亏损约 `$5.16`
- `spread >= 5c` 的交易合计亏损约 `$3.45`

因此当前 `legacy_cheap` 只接受 `spread <= 0.03` 的盘口。

### 入场价区间说明

低价不等于便宜。昨晚 `<10c` 的交易合计亏损约 `$2.58`，很多是已经接近归零的一边。

当前主策略不再用统一区间，而是按币种过滤：

```text
BTC/SOL: 0.18 <= entry_price <= 0.26
ETH:     0.20 <= entry_price <= 0.26
Others:  0.20 <= entry_price <= 0.24
```

如果 `0.20 <= entry_price < 0.25`，还需要二次过滤：

```text
spread <= 0.01
abs(dev) <= coin_max_abs_dev * 0.80
```

此外，便宜边不能明显逆着 BTC 当前 dev：

```text
买 UP   时 dev >= -0.12%
买 DOWN 时 dev <= +0.12%
```

---

## 三、仓位与风控

### 仓位

- 单笔目标投入：`$1.00`
- shares 按 `size_usdc / entry_price` 计算
- dry run 可多币种并行持仓，但不允许同一币种重复持仓
- live 模式保留单仓风控，并且只交易 BTC

### 同币种止损暂停

如果某币种当前小时触发以下退出：

- `stop_price`
- `stop_time`
- `stop_btc`
- `trailing_stop`
- `dead_water_exit`

则该币种本小时不再开新仓。新小时开始后重置。

### 全局 stop_price 熔断

如果本小时全局累计 `stop_price >= 2`，本小时停止所有新开仓。

目的：当某一小时出现集体错误方向、盘口恶化或市场急变时，避免多币种连续吃止损。

---

## 四、止盈规则

当前主策略三档分批止盈：

| 档位 | 触发价 | 卖出比例 |
|------|--------|----------|
| TP0 | 0.45 | 卖出剩余仓位的 50% |
| TP1 | 0.70 | 卖出剩余仓位的 50% |
| TP2 | 0.88 | 卖出剩余仓位的 100% |

说明：

- TP0 先锁定一半利润
- TP1 再卖剩余的一半
- TP2 清空尾仓
- live 模式 TP 使用 FAK 市价快速卖出，按真实成交 shares 和均价记账

---

## 五、退出规则

退出优先级从高到低。

### 5.1 价格止损

合约价格从入场价下跌 >= 30%：

```text
(entry_price - current_contract_price) / entry_price >= 0.30
```

触发后：

- exit_reason = `stop_price`
- 按 best bid 出场
- 该币种本小时暂停
- 本小时全局 `stop_price` 计数 +1

### 5.2 快速失败

入场 3 分钟后，如果 MFE 未启动且价格已明显走弱：

```text
elapsed_sec >= 180
max_price - entry_price < 0.02
current_price <= entry_price - 0.03
```

则退出：

- exit_reason = `fast_fail_exit`
- 用于提前处理无启动且快速走弱的仓位

### 5.3 死水早退

入场 8 分钟后，如果 MFE 未启动：

```text
elapsed_sec >= 480
max_price - entry_price < 0.02
current_price >= entry_price * 0.85
```

则退出：

- exit_reason = `dead_water_exit`
- 只处理浅亏且无启动的仓位
- 深亏仓位交给 `stop_price` 处理

### 5.4 移动止盈

如果任意 TP 已触发，残仓跌回：

```text
current_price <= entry_price + 0.02
```

则退出：

- exit_reason = `trailing_stop`
- 防止盈利仓位的尾仓回吐到接近亏损

基于持仓期间最高价 `max_price`：

| MFE 条件 | 止损线 |
|----------|--------|
| `MFE >= 0.15` | `max(max_price - 0.15, entry_price + 0.10)` |
| `MFE >= 0.10` | `entry_price + 0.05` |

当 current price 跌破对应止损线时：

- exit_reason = `trailing_stop`
- 按 best bid 出场
- 该币种本小时暂停

### 5.5 最后 10 分钟时间处理

如果进入最后 10 分钟：

| 当前价格 | 处理 |
|----------|------|
| `< 0.20` | `stop_time` 清仓 |
| `0.20 - 0.80` | 继续按止盈/止损规则 |
| `>= 0.80` | 倾向持有到期，博 $1 结算 |

---

## 六、live 与 dry run 差异

| 项目 | dry run | live |
|------|---------|------|
| 交易币种 | 多币种 | BTC only |
| 真实下单 | 否 | 是 |
| 入场 | 模拟 maker limit | 真实 BUY limit，等待 fill |
| TP 出场 | 模拟 best bid | FAK @ 0.01 floor，按真实成交记账 |
| 止损/撤退 | 模拟 best bid | FOK @ 0.01 floor，按真实成交记账 |
| 同币种重复持仓 | 禁止 | 单仓限制更严格 |

---

## 七、实验策略

当前程序还包含独立实验模拟器：

- 只做 paper 模拟，不会真实下单
- 与主策略互不影响
- API：
  - `/api/experiment/status`
  - `/api/experiment/trades`
- Dashboard 底部 `Experiment` 区块展示实验策略表现

### Experiment 1：regime

第一个实验区域用于观察 `trend / reversal / quiet_reversion` 混合 regime 策略，不作为 live 交易依据。

### Experiment 2：trend_follow

第二个实验区域只验证正向买强边，不混入 reversal / quiet。

核心逻辑：

```text
current_price > strike -> 买 UP
current_price < strike -> 买 DOWN
```

入场条件：

| 条件 | 当前规则 |
|------|----------|
| 时间窗口 | `20 < minutes_remaining <= 40` |
| 方向 | 只买强边 |
| ask 区间 | `0.65 <= ask <= 0.72` |
| spread | `ask - bid <= 0.02` |
| 挂单价格 | `entry_price = ask - 0.01` |
| 单笔成本 | `$1.00` |
| dev 阈值 | `abs(dev) >= 0.12%` |
| 波动率过滤 | 暂不启用 |
| 失速过滤 | 暂不启用 |

止盈：

| 档位 | 触发价 | 卖出比例 |
|------|--------|----------|
| TP0 | `entry + 0.10`，最高不超过 0.90 | 卖出剩余 50% |
| TP1 | `entry + 0.15`，最高不超过 0.90 | 卖出剩余 50% |
| TP2 | `0.90` | 卖出剩余 100% |

止损：

| 类型 | 当前规则 |
|------|----------|
| 价格止损 | `current_price <= entry_price - 0.08` |
| dev 动量衰减 | UP 仓位 `dev < entry_dev - 0.08%`；DOWN 仓位 `dev > entry_dev + 0.08%` |
| fast_fail | 入场 150 秒后，`MFE < 0.02` 且 `current <= entry - 0.03` |
| 死水退出 | 入场 8 分钟后，`MFE < 0.03` |
| TP 后残仓保护 | 任意 TP 触发后，`current <= entry + 0.02` 则移动止盈退出 |
| 移动止盈一档 | `MFE >= 0.08` 后，`max(entry + 0.02, max_price - 0.05)` |
| 移动止盈二档 | `MFE >= 0.12` 后，`max(entry + 0.05, max_price - 0.04)` |
| 最后 10 分钟 | `current_price < 0.60` 退出 |

风控：

- 同币种已有仓位时不再开该币种
- 同币种 stop 后，本小时不再交易该币种
- 本小时全局 `stop_price >= 3` 后停止新开仓
- live 模式下即便是实验模拟，也只观察 BTC

---

## 八、关键复盘结论

2026-05-04 晚至 2026-05-05 早的 dry run 显示：

- 原始 41 条 exit record：PnL `-$5.32`
- 新入场过滤离线估算保留 24 条：PnL `+$2.36`
- 被过滤掉 17 条：PnL `-$7.68`

主要亏损来源：

- 低价 `<10c`
- 宽 spread
- 剩余时间太少
- 多币种同时遇到弱盘口时连续 stop_price

因此当前优化方向不是换掉 `legacy_cheap`，而是降低交易频率、提高入场质量。

---

## 九、绝对红线

1. 不追涨买入超过 30c 的合约
2. 不买 entry_price < 10c 的合约
3. 不买 spread > 3c 的盘口
4. 剩余时间 <= 35min 不开新仓
5. 不加仓到亏损仓位
6. 同币种持仓未清前，不再开该币种
7. 同币种止损后，本小时不再交易该币种
8. 本小时全局 stop_price >= 2 后，停止所有新开仓
9. live 模式只允许 BTC

---

> 风险提示：以上是程序当前实现的策略规则，不构成投资建议。dry run 数据只能用于观察策略行为，不能保证 live 成交质量和收益。
