# Step 2.25 — stop_price 收紧 + dead_water 加 floor + 轮询提速

## 概要

基于 5/1 LIVE 12 笔实战数据（累计 -$8.51，10 亏 2 赢）发现策略对 Polymarket
token 价"断崖式下跌"防护不足。三处单变量改动，逐项有数据依据：

1. `stop_price` 阈值 **-50% → -30%**
2. `dead_water_exit` 加 **price floor**（current ≥ entry × 0.85 才退）
3. 持仓时轮询 **10s → 5s**

## 关键命令

```bash
cmake --build build
KEYSTORE_PASSWORD=<密码> ./build/polymarket-arb
```

## 文件清单

| 操作 | 文件 |
|------|------|
| 修改 | `src/core/strategy.cpp` — stop_price 阈值 + dead_water floor 检查 |
| 修改 | `src/main.cpp` — 主循环持仓时 sleep_sec=10→5 |
| 新增 | `docs/steps/step-2-25-report.md` — 本文 |

## 设计决策

### 1) stop_price -50% → -30%

LIVE 4 笔 stop_price 实际触发跌幅：

| entry | exit | 跌幅 | 与 -50% 阈值差距 |
|-------|------|------|-------|
| 0.21  | 0.022 | **-89%** | 跌穿 39¢ |
| 0.29  | 0.12 | -59% | 跌穿 9¢ |
| 0.25  | 0.12 | -52% | 接近阈值 |
| 0.24  | 0.08 | -67% | 跌穿 17¢ |

原因：BTC 暴动时 token 价**断崖**（不是渐变），10s 轮询间隔下跨过 -50% 阈值时
价格已经 -89%。改 -30% 让止损在更靠前位置触发，即便有 lag 也不会跌到 -50%+。

代价：浅震荡市可能在 -30% 误触发后反弹（trade-off：用确定性 vs 灵活性）。

### 2) dead_water 加 price floor

dead_water 当前条件：`elapsed >= 8min AND mfe_gain < 2¢`。**没检查 current_price**
导致 LIVE 5 笔 dead_water 中 2 笔触发时已 -37/-38% 深亏。

改：`AND current_price >= entry × 0.85`（亏 ≤ 15% 才走 dead_water；深亏让 stop_price 接管）。

dead_water 设计本意是"市场不动平局退出"，floor 让它回归本意。

### 3) 持仓时轮询 10s → 5s

stop_price 触发 lag 主因。5s 间隔是末 10min 已用值（无新依赖）。
空闲 15s 不变（无持仓时不需要快速响应）。

## 红线遵守

- [x] 没有引入硬编码密钥
- [x] 没有跳过错误处理
- [x] 编译通过，无 warning
- [x] dry_run 路径完全不受影响（策略改动同时影响 dry/live）
- [x] 每处改动有 LIVE 实战数据支撑

## 验收结果

```
编译: ok
3 处改动各 1-3 行
预估回放 12 笔 LIVE 收益（粗估）：
  · 4 笔 stop_price 收紧：节省 ~$3-5
  · 2 笔深亏 dead_water 改走 stop_price：节省 ~$1-2
  · 总预估：-$8.5 → -$3 ~ -$4.5
注：仅是回放估算；真正验证需新一周 LIVE 数据。
```

## 遗留问题

- **本质问题未解：BTC 强趋势市策略仍是逆势押便宜方**。Step 2.21 主动移除 BTC dev
  过滤，5/1 BTC 上涨 +1% 行情下 8/12 笔押 DOWN 必亏。如果连续亏损扩大，需重
  考虑加 BTC 趋势 / vol 过滤（违反 Step 2.21 偏好但保护资金）。
- 12 笔样本仍小，不能下"策略不行"结论 —— 但 stop_price 阈值过宽是确定 bug。
