# Polymarket 1小时 UP/DOWN 交易策略

> 最后更新：2026-05-09。当前主策略是 `regime_adaptive`：先判断 quiet / momentum / uncertain，再决定是否交易。优化后的 `legacy_cheap_v2` 保留为独立实验对照组。

---

## 一、市场类型

- **标的**：Polymarket Crypto 1小时 UP/DOWN 二元期权
- **监听币种**：BTC、ETH、SOL、XRP、DOGE、BNB
- **live 交易币种**：只允许 BTC
- **dry run 交易币种**：允许多币种
- **合约价格区间**：$0 - $1
- **到期结算**：胜方结算 $1，败方结算 $0

---

## 二、核心策略：regime_adaptive

`regime_adaptive` 不再直接“便宜就买”。它先按 dev 强度把市场拆成三类：

| Regime | 条件 | 动作 |
|--------|------|------|
| quiet | `abs(dev) <= 0.12%` | 允许 `cheap_rebound` |
| uncertain | `0.12% < abs(dev) < 0.20%` | 不交易 |
| momentum | `abs(dev) >= 0.20%` | 允许强边 `trend_follow` |

### cheap_rebound 入场条件

以下条件必须全部满足：

| 条件 | 当前规则 |
|------|----------|
| 时间窗口 | `36 <= minutes_remaining <= 45` |
| 方向选择 | 买入 UP / DOWN 中 ask 更低的一方 |
| ask 红线 | `candidate_ask <= 0.29` |
| 入场价区间 | BTC/ETH/SOL `0.22-0.28`，其它 `0.24-0.28` |
| spread 过滤 | `spread <= 0.01` |
| dev 过滤 | BTC/ETH/SOL `abs(dev) <= 0.12%`，其它 `<= 0.08%` |
| 跨币种背景 | 若买入方向被多数其它币种强反向，不开仓 |
| 挂单价格 | `entry_price = candidate_ask - 0.01`，最低 0.01 |
| 单笔成本 | `size_usdc = $1.00` |
| 同币种持仓 | 同一币种已有未平仓仓位时，不再开该币种 |
| live 限制 | live 模式只交易 BTC |

### momentum 入场条件

| 条件 | 当前规则 |
|------|----------|
| 时间窗口 | `32 <= minutes_remaining <= 42` |
| 方向选择 | dev 为正买 UP，dev 为负买 DOWN |
| 入场价区间 | `0.64 <= entry_price <= 0.69` |
| spread 过滤 | `spread <= 0.01` |
| dev 过滤 | `abs(dev) >= 0.20%` |
| 高价过滤 | `entry_price > 0.68` 时必须 `abs(dev) >= 0.24%` |
| 弱币种过滤 | 非 BTC/ETH/SOL 必须 `abs(dev) >= 0.24%` |

### spread 过滤说明

`spread = ask - bid`。

如果 ask 很低但 bid 更低，买入后马上按 bid 估值会出现很大的天然浮亏。昨晚日志显示：

- `spread >= 3c` 的交易合计亏损约 `$5.16`
- `spread >= 5c` 的交易合计亏损约 `$3.45`

因此当前主策略和 `legacy_cheap_v2` 实验对照组都只接受 `spread <= 0.01` 的入场。

### 入场价区间说明

低价不等于便宜。昨晚 `<10c` 的交易合计亏损约 `$2.58`，很多是已经接近归零的一边。

当前主策略不再用统一区间，而是按 regime 过滤：

```text
cheap_rebound:
  BTC/ETH/SOL: 0.22 <= entry_price <= 0.28
  Others:      0.24 <= entry_price <= 0.28

momentum:
  Strong side: 0.64 <= entry_price <= 0.69
```

`legacy_cheap_v2` 实验对照组继续保留按币种区间、`0.20-0.24` 二次过滤和 confidence 评分，用于和新主策略比较。


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

cheap_rebound 三档分批止盈：

| 档位 | 触发价 | 卖出比例 |
|------|--------|----------|
| TP0 | 0.45 | 卖出剩余仓位的 50% |
| TP1 | 0.70 | 卖出剩余仓位的 50% |
| TP2 | 0.88 | 卖出剩余仓位的 100% |

momentum 分支使用更近的动态止盈：

| 档位 | 触发价 | 卖出比例 |
|------|--------|----------|
| TP0 | `min(0.88, entry + 0.08)` | 卖出剩余仓位的 50% |
| TP1 | `min(0.90, entry + 0.14)` | 卖出剩余仓位的 50% |
| TP2 | 0.90 | 卖出剩余仓位的 100% |

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

`quiet_reversion` 入场后如果没有启动，会更早退出：

```text
elapsed_sec >= 20
max_price - entry_price < 0.015
current_price <= entry_price - 0.02
```

或者：

```text
max_adverse >= 0.06
max_price - entry_price < 0.10
```

对应退出原因：

- `cheap_fail_stop`
- `adverse_expansion_stop`

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

### Experiment 1：legacy_cheap_v2

第一个实验区域用于保留优化后的 `legacy_cheap` 作为对照组，不作为 live 交易依据。它继续使用旧版便宜边逻辑和近期加入的过滤：

- `minutes_remaining > 35`
- 按币种 entry 区间过滤
- spread 必须 `<= 0.01`
- `0.20 <= entry < 0.25` 二次过滤
- `cheap_rebound_confidence >= 3`
- `entry >= 0.25` 时要求 `cheap_rebound_confidence >= 4`
- confidence 加分：`41-45 min`、tight spread、`0.20-0.24` entry、核心币、温和 dev
- confidence 扣分：非 `41-45 min`、非核心币、背景反向、BTC/ETH 分歧
- `cheap_fail_stop`
- `adverse_expansion_stop`
- TP 后残仓保护

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
| 时间窗口 | `30 < minutes_remaining <= 42` |
| 方向 | 只买强边 |
| ask 区间 | `0.65 <= ask <= 0.72` |
| spread | `ask - bid <= 0.02`；高价入场必须 `<= 0.01` |
| 挂单价格 | `entry_price = ask - 0.01` |
| 单笔成本 | `$1.00` |
| dev 阈值 | `abs(dev) >= 0.18%` |
| 波动率过滤 | 暂不启用 |
| 高价过滤 | `entry_price > 0.68` 时，必须 `abs(dev) >= 0.22%` 且 spread `<= 0.01` |
| 置信度过滤 | `entry_confidence >= 3` |

置信度评分：

```text
+1 目标币方向确认
+1 连续同方向确认
+1 BTC aligned / neutral
+1 ETH aligned / neutral
+1 abs(dev) >= 0.22%
+1 entry_price <= 0.68
-1 entry_price > 0.69
-1 BTC/ETH 明显分歧
-1 BTC 强反向
```

`entry_price > 0.68` 时还要求 `entry_confidence >= 4`。

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
| momentum_fail_stop | 90 秒后 `MFE < 0.015` 且 `current <= entry - 0.02`；或 150 秒后 `MFE < 0.03`；或 BTC/ETH 背景反向且未浮盈 |
| 死水退出 | 入场 8 分钟后，`MFE < 0.03` |
| TP 后残仓保护 | 任意 TP 触发后，`current <= entry + 0.02` 则移动止盈退出 |
| 移动止盈一档 | `MFE >= 0.08` 后，`max(entry + 0.02, max_price - 0.05)` |
| 移动止盈二档 | `MFE >= 0.12` 后，`max(entry + 0.05, max_price - 0.04)` |
| 最后 8 分钟 | `current_price < 0.78` 或 dev 不再同向则退出 |
| 最后 5 分钟 | 除非 `current_price >= 0.88` 且 dev 仍同向，否则退出 |

风控：

- 同币种已有仓位时不再开该币种
- 同币种 stop 后，本小时不再交易该币种
- 本小时全局 `stop_price >= 3` 后停止新开仓
- live 模式下即便是实验模拟，也只观察 BTC
- 实验日志记录 `entry_confidence`、BTC/ETH alignment、`cross_coin_state`、entry bucket、MFE/MAE 和 confidence components

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

因此当前优化方向已经从单一 `legacy_cheap` 升级为 regime-based：quiet 才做便宜边反弹，momentum 才做强边跟随，中间不确定区间不交易。

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
