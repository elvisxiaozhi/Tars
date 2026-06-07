# Step — held-to-expiry shadow 双策略（held_favorite_v1 + held_momentum_v1）

## 概要

新增第 6、7 个 shadow 实验 `held_favorite_v1` 与 `held_momentum_v1`，各自独立运行 + 各自 dashboard TAB。两者共享一个结构性论点：**"持有到期"绕开手续费杀手**——入场限价 maker(免费) + 持有到 1h 结算(0/1，到期无离场费) → 近零费。这是对"现有策略毛边际盖不过 ~7% taker 费"诊断的正面回应。**纯 shadow，零影响 main / trend_v2 / trend_v3**（独立 ExperimentEngine、独立持仓/余额/jsonl）。

## 动机

近几天服务器数据复盘（见对话分析）确认：main/trend_v2/trend_v3 胜率下降是 **regime**（毛胜率级别同步暴跌，手续费数学上不可能影响毛胜率），而真实费把本就薄的毛边际推成净亏；止损 taker 费是大头且不可避免。结论：信号微调属低确认（postmortem 已证 vol/dev/side/hour 不分离赢家），**唯一结构性解药是"持有到期"消掉离场费**。两个候选验证两种过费方式：

- **held_favorite**：高胜率/低 R:R——晚段买已确定的赢家，靠胜率 > 入场价。
- **held_momentum**：较好 R:R/中胜率——中段买便宜顺势侧，靠 move 延续。

## 设计

| 维度 | held_favorite_v1 | held_momentum_v1 |
|---|---|---|
| 币种 | 仅 BTC | 仅 BTC |
| 时间窗 | 剩余 2–12 min（晚段） | 剩余 20–40 min（中段） |
| 方向 | favorite（ask 更高侧），dev 须同向 | 顺势侧（dev 方向），\|dev\| ≥ 0.08% |
| 入场价带 | 0.80–0.93 | 0.45–0.62 |
| 价差上限 | ≤ 3¢ | ≤ 2¢ |
| 出场 | **无 TP / 无止损，持有到 1h 结算(0/1)** | 同左 |
| 费用 | 入场 maker≈0 + 到期免费 → 近零费 | 同左 |
| 仓位 | $1 名义（经 5 股/$1 下限托底，沿用真实化口径） | 同左 |
| 落盘 bucket | `fav_0.80_0.85` / `0.85_0.90` / `0.90_0.93` | `mom_0.45_0.52` / `mom_0.52_0.62` |

**实现机制**：两者 `tp_levels` 为空、exit 恒 `should_exit=false`；持仓一直挂到 `minutes_remaining<=0` 或 market 变 stale，由现有 `close_expired` 按 `md.current_price vs strike` 结算 0/1（crypto 分支，无费）。复用现有 5 股/$1 入场下限 + maker 入场记账。

## 文件清单（全部加法，无删改 main / trend_v2 / trend_v3 逻辑）

| 操作 | 文件 | 内容 |
|------|------|------|
| 修改 | `src/core/experiment_engine.h` | 声明 `evaluate_held_favorite_v1` / `evaluate_held_momentum_v1` |
| 修改 | `src/core/experiment_engine.cpp` | 实现 2 个 evaluate + 3 dispatcher 分支（entry / TP=空 / exit=no-op） |
| 修改 | `src/main.cpp` | 2 实例 + 6 API 回调 + 2 reset_candle + 2 reset_global_hour + 2 on_market |
| 修改 | `src/net/api_server.{h,cpp}` | `/api/experiment-held-favorite/*` + `/api/experiment-held-momentum/*`（各 status/trades/all-trades） |
| 修改 | `src/dashboard.h` | "Held Favorite" + "Held Momentum" 两个 TAB + 面板 + fetch + render（复用 `renderExperiment`） |
| 修改 | `docs/strategy-experiments.md` | 5→7 活跃实验，总表 + 描述 |
| 新增（运行时） | `logs/experiment_held_{favorite,momentum}_v1_{trades,candidates}.jsonl` | 自动创建 |

## 设计决策

- **id 前缀 `K`（held_favorite）/ `M`（held_momentum）**：与现有 F/H/D/T/V 不冲突。
- **入场维持 maker（限价 ask−1¢）而非市价**：与框架一致；近零费的主因是"无离场费"，入场 maker/taker 二阶。入场不填它就是和其它实验同样的"限价必成交"假设，口径一致不额外乐观。
- **v1 不加 dev-accel 门**：价带本身已编码大部分确定性；先用干净的简单版出样本，若 held_momentum 分桶胜率不达标再考虑 v2 加趋势确认（避免过拟合小样本）。
- **判据先定死（防过拟合）**：按入场价分桶看"真实到期胜率 vs 入场价"，只有稳定 > 入场价的桶才算 edge；提拔门槛 = net 为正 + 样本 ≥ ~80-100 + 跨 regime 翻面仍正。

## 红线遵守

- [x] 没有引入硬编码密钥
- [x] 没有跳过错误处理
- [x] 编译通过、链接成功，无新 warning（api_server 既有 L161/L221 lint 与本步无关）
- [x] 二进制 grep 确认含两策略逻辑 + 4 路由 + reject 原因；dashboard 两 tab/面板齐全；KAT(test_crypto/chain/eip712) 全过

## 诚实声明（不吹）

- **这是假设，不是已验证的赚钱策略**。论点（市场低估晚段 favorite / move 延续）需前向 shadow 出样本才算数——现有日志没记到期结果，无法干净回测。
- **held_favorite 是肥尾风险**（罕见尾盘急反转单笔大亏 −0.85+），靠样本量确认胜率真的 > 入场价。
- 把握度：中等。成功=某价桶实测真实胜率稳定 > 入场价；失败=市场对这两类已有效定价 → 零真钱损失，结论"此路不通"。

## 验收结果

```
cmake --build build -j  →  [100%] Built target polymarket-arb（无新 warning）
strings build/polymarket-arb | grep: held_favorite_v1×3 / held_momentum_v1×3 / experiment-held-favorite×4 / experiment-held-momentum×4 ✓
dashboard.h: data-tab="held-fav"/"held-mom" + tab-held-fav/tab-held-mom + held-{fav,mom}-exp-balance ✓
KAT: test_crypto OK / test_chain OK / test_eip712 OK
```

## 部署计划（等 review 后）

按 trend_v3 SOP（dashboard 分工：API 路由在 bot 二进制需重编；dashboard.h 由 manager 运行时读取需重启 manager）：
1. scp 改动的 6 文件（experiment_engine.{h,cpp}、main.cpp、api_server.{h,cpp}、dashboard.h）。
2. 服务器 `cmake --build build -j2` 重编 → 停 bot → 起 bot（manager API）。
3. `systemctl restart polymarket-manager.service` 让 manager 重读 dashboard.h。
4. 验证：`/api/experiment-held-favorite/status` 返回 JSON、`logs/experiment_held_favorite_v1_candidates.jsonl` 出现并增长、dashboard 出现两个新 tab。

## 后续 / 成功判据

1. 攒 ~80-100 仓后：按 `entry_price_bucket` 分桶看真实到期胜率 vs 入场价；任一桶稳定 > 入场价 = 有 edge。
2. held_favorite 若胜率不及入场价 → 市场已有效定价，退役。
3. held_momentum 若简单版分桶不达标 → 可加 dev-accel 趋势确认门（v2）再试一轮，仍不行则退役。
4. 任一证明跨 regime net 正 → 才考虑 live 提拔（另开 step + 风控复核）。
