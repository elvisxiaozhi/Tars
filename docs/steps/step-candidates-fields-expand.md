# Step candidates-fields-expand — 扩充 main_candidates.jsonl 诊断字段

## 概要

只动 instrumentation：`log_main_candidate` 多写 8 个字段，便于后续主策略反事实回放与策略阈值假设验证。**不改任何交易/入场/退出逻辑**。

## 背景

主策略当前 6f54fcc 代码版本只有 N=3 仓位、累计样本 N=20。要继续跑数据再决策，但现有 `main_candidates.jsonl` 字段不足以验证后续可能的优化假设：

- 无法回放 `cross_coin_opposed` 规则（缺其他币的偏离快照）
- 无法看 `confidence` 各分项的实际值（只有总分隐式存在）
- 无法区分 candidate 走的是 cheap_rebound 还是 momentum 路径
- 缺波动率上下文

此次只补字段，保证未来积累的数据足以做"假设是否成立"的验证。

## 关键命令

```bash
cmake --build build -j
./build/test_crypto && ./build/test_chain && ./build/test_eip712
# 本地短跑 25s 验证 JSON 形态
./build/polymarket-arb config/config.json
```

## 文件清单

| 操作 | 文件 |
|------|------|
| 修改 | `src/main.cpp` |

具体改动：

1. `log_main_candidate` 签名变更：
   - 旧：`(coin, dev_pct, minutes_remaining, sig, q)`
   - 新：`(coin, md, sig, q, market_ctx)` —— md 提供 dev/mins/vol，market_ctx 提供跨币
2. 在 log_main_candidate 前加 `regime_name` forward declaration（regime_name 定义在它之后）
3. main.cpp:1712 调用点同步更新

## 新增 JSON 字段

| 字段 | 类型 | 来源 |
|---|---|---|
| `regime_path` | string | `regime_name(sig.regime)`：cheap_rebound / trend / quiet_reversion / none |
| `entry_confidence` | int | `sig.entry_confidence`（cheap_rebound 的置信度总分） |
| `confidence_components` | string | `sig.confidence_components`，如 `"+1:time_41_45,+1:spread_tight,..."`，早期 reject 时为空 |
| `entry_vol_1h` | float | `md.current_1h_vol`（当前 1h K 线 high-low/open）|
| `avg_vol_24h` | float | `md.avg_24h_vol` |
| `cross_coin_dev` | object | `{coin: deviation_pct}` 5 个其他币偏离快照 |
| `up_bid` / `down_bid` | float | `quotes` 的 bid 价（旧 schema 只能从 ask-spread 反推） |
| `up_ask` / `down_ask` | float | 同上 |

老字段全部保留，向后兼容（旧分析脚本无需改动）。

## 设计决策

- **不存其他币的 entry_price / ask**：每 tick 6 个币、每币入场窗内多次 evaluate，会让 candidates 体量指标爆增。只存 dev 足以验证跨币假设。
- **不加 `up_spread` 旁路字段同时保留**：旧字段 `up_spread`/`down_spread` 留着，方便老脚本继续读。
- **`confidence_components` 不解析成结构**：保留原字符串，下游 Python 自己 split 即可——这样新增/删除分项不需要改 schema。

## 红线遵守

- [x] 不改任何策略入场/退出逻辑
- [x] log_main_candidate 仍包在 `try { ... } catch (...) {}` 里，落盘失败不影响主循环
- [x] 编译通过，无 warning
- [x] KAT 测试全过（test_crypto / test_chain / test_eip712）
- [x] 本地短跑验证字段实际写入

## 验收结果

```
$ cmake --build build -j
[100%] Built target polymarket-arb

$ ./build/test_crypto    →  40 passed
$ ./build/test_chain     →  ALL PASS
$ ./build/test_eip712    →  === ALL PASS ===

$ (./build/polymarket-arb config/config.json &) ; sleep 25 ; pkill ...
$ tail -1 logs/main_candidates.jsonl
{
  "coin": "XRP",
  "regime_path": "trend",
  "reject_reason": "momentum_time_window",
  "deviation_pct": -0.269,
  "cross_coin_dev": {"BNB": -0.264, "BTC": -0.342, "DOGE": -0.038, "ETH": -0.253, "SOL": -0.070},
  "entry_confidence": 0,
  "confidence_components": "",
  "entry_vol_1h": 0.00836,
  "avg_vol_24h": 0.00639,
  "up_bid": 0.13, "up_ask": 0.14, "down_bid": 0.86, "down_ask": 0.87,
  ...
}
```

## 部署影响

- 字段是 append-only，旧分析脚本仍可工作（只读自己关心的字段）
- candidates 文件大小会略增（每行多约 200 字节）。当前 9000 行 ≈ 2.7MB → 增至约 4.5MB，可接受
- 不需要重置任何状态，热部署即可（manager 重启 / systemd restart）

## 遗留问题

- 后续可考虑也在 `trades.jsonl` 落盘时打入 cross_coin_dev 快照（用作 entry 时的"上下文凝固"）。本次只动 candidates，保持改动最小。
- `confidence_components` 字段对早期 reject（如 coin_filter）为空字符串，符合代码实际行为——下游分析需注意"空 = 未计算"而非"零分"。
