# 运行时日志速查手册

> 适用：live-trading 分支，spdlog 输出格式 `[时间戳] [级别] 内容`
> 级别颜色：`info` 白 / `warning` 黄 / `error` 红 / `debug` 灰（默认不显示）

---

## 一、快速索引表

| 关键词 | 级别 | 含义 | 详见 |
|--------|------|------|------|
| `BTC: $...` | info | BTC 行情快照（每 ~30s） | §二 |
| `#N \| ...min \| BTC` | info | 策略状态行（每 tick） | §三 |
| `rejects ×N:` | info | 拒绝原因聚合统计 | §四 |
| `Signal blocked:` | debug | RiskManager 拦截（默认不显示） | §四 |
| `SIGNAL:` | info | 产生有效入场信号 | — |
| `LIVE BUY placed` | info | FOK 买单已发，等待 fill | — |
| `LIVE BUY filled` | info | 买单成交 ✓ | — |
| `LIVE BUY didn't fill` | warning | 买单超时未成交，已取消 | — |
| `OPEN [...]` | info | 仓位开仓记录 | — |
| `TP0/TP1/TP2 [...]:` | info | 止盈减仓成功 | §五 |
| `ALL TP FILLED` | info | 三档止盈全部完成 | §五 |
| `STOP LOSS (price):` | warning | 价格硬止损触发 | §五 |
| `DEAD WATER EXIT:` | warning | 死水早退触发 | §五 |
| `TRAILING STOP (peak-15c):` | warning | 高位移动止盈触发 | §五 |
| `TRAILING STOP (entry+5c):` | warning | 低位移动止盈触发 | §五 |
| `TIME STOP:` | warning | 最后10分钟时间止损 | §五 |
| `CLOSE [...]` | info | 仓位平仓记录（含 reason） | §五 |
| `Candle stopped:` | warning | 本场蜡烛锁定，不再开新仓 | §六 |
| `New candle: ...→ reset` | info | 新蜡烛到来，锁定解除 | §六 |
| `POLY: cash=` | info | Polymarket 账户余额刷新 | — |
| `EMERGENCY CLOSE ALL` | warning | 紧急平仓流程启动（SIGINT 等） | §七 |
| `EMERGENCY done:` | warning | 紧急平仓完成 | §七 |
| `EMERGENCY: not authenticated` | error | 紧急平仓失败，未认证 | §七 |

---

## 二、BTC 行情日志

```
[info]  BTC: $77059.27 | strike: $77131.08 | dev: -0.09% | vol: 0.0072 (avg: 0.0046) | 22min left
```

| 字段 | 含义 |
|------|------|
| `$77059.27` | 当前 BTC 现货价 |
| `strike: $77131.08` | 本小时蜡烛的到期 strike 价 |
| `dev: -0.09%` | 现价与 strike 的偏差百分比，负 = BTC 低于 strike |
| `vol: 0.0072` | 当前实时波动率（用于判断是否适合入场） |
| `avg: 0.0046` | 近期平均波动率（对比基准） |
| `22min left` | 距本场蜡烛到期剩余分钟 |

---

## 三、策略状态行

每个 tick 结束时打印一次，格式随是否持仓而变化。

### 空仓时
```
[info]  #62 | 22min | BTC $77059 -0.09% | Up 0.25 Dn 0.32 | idle | $15.71 | 10s
```

| 字段 | 含义 |
|------|------|
| `#62` | tick 序号（从本次启动开始计数） |
| `22min` | 距到期剩余分钟 |
| `BTC $77059 -0.09%` | BTC 现价 + 与 strike 偏差 |
| `Up 0.25 Dn 0.32` | 当前 UP 合约 ask / DOWN 合约 ask |
| `idle` | 当前无持仓 |
| `$15.71` | 当前账户余额 |
| `10s` | 本 tick 循环耗时（秒） |

### 持仓时
```
[info]  #62 | 22min | BTC $77059 -0.09% | P1 UP 0.25->0.32 +28% MFE:0.36 rem:100% | $15.71 | 10s
```

| 字段 | 含义 |
|------|------|
| `P1` | 仓位编号 |
| `UP` | 仓位方向（UP 或 DOWN） |
| `0.25->0.32` | 入场价 → 当前价 |
| `+28%` | 当前浮盈百分比 |
| `MFE:0.36` | 历史最高价（Maximum Favorable Excursion） |
| `rem:100%` | 剩余未卖出仓位百分比（TP 减仓后会下降） |

---

## 四、拒绝原因统计

### 日志格式
```
[info]  rejects ×9: ask_too_high×9
[info]  rejects ×8: ask_too_high×1 no_entry_window×7
```

### 打印时机
满足以下**任一条件**才批量 flush，不是每 tick 都打：
- 累积拒绝次数 **≥ 10 次**
- 距上次 flush 超过 **5 分钟**

### 第一层：Strategy 拒绝（计入 rejects 统计）

| 原因 | 触发条件 | 能否自行恢复 |
|------|---------|------------|
| `time_too_short` | 剩余时间 ≤ 20 分钟 | 否，等下一场蜡烛 |
| `no_entry_window` | 时间窗口计算结果无效 | 随时间自动变化 |
| `no_valid_ask` | UP/DOWN 两边均无挂单 | 等市场流动性恢复 |
| `invalid_ask` | ask ≤ 0 或 ≥ 1.0 | 等市场恢复 |
| `ask_too_high` | 当前 ask 超过动态最高入场价 | 等价格回落或时间推进 |
| `red_line_30c` | ask > 0.30，硬红线 | 否，该场不会入场 |

> `ask_too_high` 是最常见原因：剩余时间越多，允许的最高入场价越低（保护利润空间）；随着时间流逝阈值会上移，有时会自动解除。

### 第二层：RiskManager 拒绝（打 `Signal blocked` debug，不计入 rejects）

| 原因 | 触发条件 | 能否自行恢复 |
|------|---------|------------|
| `candle_stopped` | 本场已触发止损性出场 | 否，等下一场蜡烛 |
| `max_concurrent` | 已持有 1 个仓位（单仓制） | 等现有仓位平仓后 |
| `insufficient_balance` | 余额不足支付本次开仓成本 | 否，需人工充值 |

---

## 五、出场类型详解

所有出场都会在最终打印 `CLOSE [mode] id reason=X | pnl=... | balance=...`。

### 止盈出场（TP）— info 级，不锁蜡烛

```
[info]  TP0 [UP]: sell 3.0 shares @ 0.450 | pnl=+$0.60 | remaining=70% | balance=$16.31
[info]  TP1 [UP]: sell 3.0 shares @ 0.700 | pnl=+$1.35 | remaining=40% | balance=$17.66
[info]  TP2 [UP]: sell 4.0 shares @ 0.900 | pnl=+$2.60 | remaining=0%  | balance=$20.26
[info]  ALL TP FILLED [UP]: total_rpnl=+$4.55 | balance=$20.26
```

| 档位 | 价格目标 | 卖出比例 | 说明 |
|------|---------|---------|------|
| TP0 | 0.45 | 30% | 先锁住中间浮盈，防止全回吐 |
| TP1 | 0.70 | 剩余的 ~43%（原始 30%） | 中段利润 |
| TP2 | 0.90 | 全部剩余 | 博取最大收益 |

### 止损性出场 — warning 级，**触发后锁蜡烛**

**① STOP LOSS (price)** — 价格硬止损
```
[warning]  STOP LOSS (price): Bitcoin Up or Down... loss=52.0% entry=0.250 now=0.120
```
- 触发：`(entry - now) / entry ≥ 50%`，即价格腰斩
- 优先级最高，立即 market order 出场

**② DEAD WATER EXIT** — 死水早退
```
[warning]  DEAD WATER EXIT: Bitcoin Up or Down... elapsed=312s mfe_gain=+0.030 entry=0.250 now=0.240
```
- 触发：入场满 5 分钟，且 MFE 涨幅 < 5¢
- 含义：方向没有启动迹象，提前小亏离场比拖到止损要好

**③ TRAILING STOP (peak-15c)** — 高位移动止盈
```
[warning]  TRAILING STOP (peak-15c): Bitcoin Up or Down... entry=0.250 peak=0.420 now=0.260 stop=0.270
```
- 触发条件：MFE ≥ 15¢（曾有较大涨幅）
- 止损线：`max(峰值 - 15¢, 入场价 + 10¢)`
- 保底至少锁住 +10¢ 利润

**④ TRAILING STOP (entry+5c)** — 低位移动止盈
```
[warning]  TRAILING STOP (entry+5c): Bitcoin Up or Down... entry=0.250 peak=0.360 now=0.290 stop=0.300
```
- 触发条件：MFE ≥ 10¢ 且 < 15¢（轻度盈利）
- 止损线：`入场价 + 5¢`
- 只要曾盈利过 10¢，确保至少锁住 5¢

**⑤ TIME STOP** — 最后 10 分钟时间止损
```
[warning]  TIME STOP: price=0.150 < 20¢ with 8min left
```
- 触发：剩余 ≤ 10 分钟且 price < 20¢
- 含义：快到期还在低位，方向判断错误，止损离场
- 反向情况（price ≥ 80¢）：打 `HOLD TO EXPIRY` debug 日志，持有等 $1 结算

---

## 六、蜡烛锁定机制

### 触发锁定
以下 4 种止损性出场**都会**触发蜡烛锁定：

```
[warning]  Candle stopped: trailing_stop triggered, no more trades this candle
[warning]  Candle stopped: stop_price triggered, no more trades this candle
[warning]  Candle stopped: stop_time triggered, no more trades this candle
[warning]  Candle stopped: dead_water_exit triggered, no more trades this candle
```

锁定后，RiskManager 拒绝所有新开仓请求（`candle_stopped` 原因），直到下一场蜡烛。

**TP 止盈正常出场不锁蜡烛。**

### 解锁
下一场蜡烛到来时（market slug 变化）自动解除：
```
[info]  New candle: Bitcoin Up or Down - May 1, 1AM ET → reset stop flag
```

---

## 七、紧急平仓流程

### 触发时机
- 程序收到 **Ctrl+C / SIGINT / SIGTERM** 且当前有持仓

### 完整日志序列

```
[warning]  ===== EMERGENCY CLOSE ALL =====

[warning]    [step1] cancelling 1 open order(s)       ← 先取消未成交的挂单
[warning]      0x1a2b3c...  ✅ cancelled
[warning]      0x4d5e6f...  ⚠️ FAILED                 ← 失败但继续执行 step2

[warning]    [step2] selling 1 position(s) FOK        ← 对所有持仓发市价卖单
[warning]      SENT  7540267...  shares=10.0000  order_id=0x2d348a...
[error]       FAIL  7540267...  错误信息               ← 发单被拒
[error]       THREW 7540267...  异常信息               ← 发单抛出异常

[warning]  ===== EMERGENCY done: 1 sell(s) sent =====
```

### 关键设计
| 细节 | 原因 |
|------|------|
| Step1 先取消挂单 | 防止 emergency 触发时 BUY 单正在 fill polling，先 cancel 避免形成新持仓 |
| SELL 价格 = $0.01 | FOK 挂极低价，server 按对手最佳 bid 立即成交，等效市价单 |
| 不等 fill 确认 | fire-and-forget，程序即将退出，无法轮询 |
| 未认证时直接 error | `clob_authenticated_=false` 时无法发单，打 error 后返回 |

> `EMERGENCY done: N sell(s) sent` 中的 N 是**发单次数**，不是成交次数。实际成交需查链上记录。
