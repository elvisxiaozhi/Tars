# Step — 退役 trend_v3 + held_favorite_v1 + held_momentum_v1（动量延续族无 edge）

## 概要

退役 3 个 shadow 实验:`trend_v3`、`held_favorite_v1`、`held_momentum_v1`。三者与 main/trend_v2 同属**"买顺势 favorite、赌 BTC 这小时 move 延续"**的同一个赌,真实费 era 数据证明这一族在市场隐含概率上**结构性零 edge**。退役 = 不再实例化 + 移除 dashboard tab + API 路由;`evaluate_*` 代码按惯例保留作回放参考。保留 main + trend_v2 作 benchmark。

## 证据（真实费 era,各跑数天）

| 策略 | 仓 | 净 | 毛(扣费前) | 净胜率 | avg入场 |
|---|---|---|---|---|---|
| main | 48 | −4.90 | −1.19 | 35.4% | 0.657 |
| trend_v2（保留） | 124 | −8.78 | +0.66 | 34.7% | 0.655 |
| **trend_v3（退役）** | 88 | −8.28 | −2.10 | 36.4% | 0.706 |
| **held_momentum（退役）** | 21 | −17.90 | −17.90 | 42.9% | 0.599 |
| **held_favorite（退役）** | 0 | — | — | — | 窗口够不到 |

- **带管理的(main/v2/v3):毛 edge≈0**(最好 trend_v2 毛 +$0.66/124≈每笔 0),净负全因 ~7% taker 费。
- **纯持有的(held_momentum):直接负**——二元 EV = 真实胜率 − 入场价 = 0.43 − 0.60 = **−0.17/股**。favored 侧 hold-to-expiry 只赢 43%,即"中段 move 到收盘更多反转、非延续"。
- **held_favorite 0 成交**:BTC 1h 行情数据下限 mr≈21(bot 距收盘 ~20min 切走当前小时盘),其 2–12min 窗口永远触发不了。
- **结论**:这一族买在市场隐含概率上无 edge;trend_v3 调价带/held 移窗口都只是同一个无 edge 赌的换皮。

## 文件清单

| 操作 | 文件 | 内容 |
|------|------|------|
| 修改 | `src/main.cpp` | 移除 3 个 ExperimentEngine 实例 + 9 API 回调 + reset_candle/reset_global_hour/on_market 各 3 行;留退役注释 |
| 修改 | `src/net/api_server.{h,cpp}` | 移除 9 个 `on_*_experiment_*` 声明/成员/路由/setter |
| 修改 | `src/dashboard.h` | 移除 3 个 tab 按钮 + 3 个面板 + fetch(destructure+Promise.all) + render |
| 保留 | `src/core/experiment_engine.{h,cpp}` | `evaluate_trend_v3 / evaluate_held_favorite_v1 / evaluate_held_momentum_v1` + dispatch 分支**保留**作回放参考(不实例化即不触发) |
| 修改 | `docs/strategy-experiments.md` | 7→4 活跃,3 行从总表移除,加退役记录 + "不要重做这一族"判据 |

## 设计决策

- **保留 evaluate_* 代码**:沿用 eth_cheap/eth_late_cheap 退役惯例(回放参考);只去实例化与 UI/路由,避免误用 = 不再 new。
- **dashboard tab + API 路由一并移除**:与 eth_cheap 退役一致,避免死 tab 与无效请求。
- **文档写死"不要再做这一族"**:按用户要求,把退役理由 + 判据留档,防后续重复造同族变体(等同 cheap-value 失败族的处置)。

## 红线遵守

- [x] 没有引入硬编码密钥
- [x] 没有跳过错误处理
- [x] 编译通过、链接成功（既有 api_server bugprone-unused-return-value lint 与本步无关）
- [x] main / trend_v2 / finance / crypto_4h / crypto_daily 逻辑零改动
- [x] dashboard grep 确认:tab 5 个(main/trend-v2/crypto-4h/crypto-daily/finance)、0 处残留 retired 引用

## 验收结果

```
cmake --build build -j → [100%] Built target polymarket-arb（无新 warning）
dashboard tab-btn: main / regime / crypto-4h / crypto-daily / finance（trend-v3/held-fav/held-mom 已移除）
grep 'trend-v3|held-fav|held-mom' src/dashboard.h → 0
```

## 部署计划（等 review 后）

按 SOP:scp 改动文件(main.cpp、api_server.{h,cpp}、dashboard.h)→ 服务器重建 → 停/起 bot(manager API)。dashboard.h 由 manager 运行时读取,无需重启 manager。退役后服务器上 `experiment_held_*` / `experiment_trend_v3_*` 的旧 jsonl 保留作历史,不再增长。

## 遗留 / 后续

- 唯一未被证伪、且数据指向 +EV 的方向是 **contrarian-hold-to-expiry(买 underdog 便宜侧、持有到期)**——但低确认(n=21、p≈0.05)且大概率 regime 依赖。作为下一步(B)单独设计 + 预设判据验证,不在本步。
