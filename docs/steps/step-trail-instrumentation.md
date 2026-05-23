# Step — Trail 评估埋点（armed_at_ms + min_price_after_arm）

## 概要

给 Position / TradeRecord 加两个观测字段,只记录、不改任何交易逻辑。目的:解决"QR breakeven-trail 到底净正还是净负"无法从现有日志判定的问题。

## 背景:为什么现有日志不够

用 104 笔影子样本(`eth_cheap_v1`)反演 trail 时发现:**43 个赢家全部在某刻触及过 trail 线(entry+0.02)**。逆势 cheap-value 的典型路径是"买入后先更跌→再反弹到 0.5+"。trail 会不会误杀赢家,取决于"回踩 trail 线"发生在**武装(到达 entry+0.05)之前还是之后**——而日志只有全局 max/min,无法还原这个时序。

净收益边界因此极宽,且跨越零:

```
                乐观(只救亏家)   悲观(同时砍赢家)
arm=0.05 (已部署)    +4.06          -12.08
arm=0.03            +8.40           -7.74
```

已部署的 arm=0.05 trail 可能是净负的。必须埋点才能判定。

## 埋点字段

| 字段 | 含义 |
|---|---|
| `armed_at_ms` | 首次到达 `entry+0.05`(已部署 trail 的 arm 参考线)的 wall-clock ms;`0`=从未武装 |
| `min_price_after_arm` | 武装**之后**出现的最低 bid |

判定逻辑(离线):对每笔影子赢家,若 `min_price_after_arm ≤ entry+0.02` → 它在武装后确实回踩到了 trail 线却仍走成赢家 → **trail 一定会误杀它**。反之 trail 安全。亏家同理可算被救额。影子策略没有 trail,价格路径完整,所以这个反演是干净的。

## 文件清单

| 操作 | 文件 |
|------|------|
| 修改 | `src/core/strategy.h` — Position 加 2 字段 |
| 修改 | `src/core/trade_journal.h` — TradeRecord 加 2 字段 |
| 修改 | `src/main.cpp` — 持仓循环跟踪武装 + `fill_analytics` 拷贝 |
| 修改 | `src/core/experiment_engine.cpp` — on_market 循环跟踪 + `fill_record_analytics` 拷贝 + `append_jsonl` 序列化 |
| 修改 | `src/core/trade_journal.cpp` — 写 jsonl + 读回(round-trip) |

## 设计决策

- **只埋 arm=0.05 单一参考线**:这是当前**已部署**的 trail 参数,即生产里真实存在的风险。不为假设的 0.03 多埋(避免 over-build);将来要评 0.03 再加第二条参考线。
- **主策略 + 影子都埋**:主策略的 trail 会触发即平仓,看不到反事实;**真正能反演的是影子**(无 trail,路径跑完)。影子成交量大(eth_cheap_v1 ~100 笔/周),约 1 周即可给 trail 一个确定答案。
- **纯观测,零行为改动**:不动入场/出场/TP/风控。`armed_at_ms==0` 哨兵表示从未武装,旧数据读回默认 0,向后兼容。

## 红线遵守

- [x] 没有引入硬编码密钥
- [x] 没有跳过错误处理
- [x] 编译通过(本地);test_crypto / test_eip712 PASS(test_chain 需代理略过)
- [x] 不改 V2 字段 / 三层账户 / 代理 / 单仓制 / 任何交易逻辑

## 验收结果

```
cmake --build build -j  → 100% Built target polymarket-arb
test_crypto: PASS
test_eip712: PASS
```

部署后真实验收:下一笔成交的 jsonl 行应含 `armed_at_ms` 和 `min_price_after_arm` 两个新 key。

## 遗留问题

1. **需部署到服务器**才能开始积累(影子策略在跑)。仍受"本机无法交叉编译"限制,部署需再次服务器编译。
2. 积累约 1 周(~100 笔影子)后,用 `min_price_after_arm` 给"已部署 arm=0.05 trail 是否净负"下确定结论;届时决定保留 / 调参 / 回滚。
3. 若结论是 trail 净负,需回滚 strategy.cpp 里 QR 的 `qr_breakeven_trail` 分支。
