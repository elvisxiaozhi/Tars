# Step — trend_v3 shadow 策略（dev-accel 门解锁高价带）

## 概要

新增第 5 个 shadow 实验 `trend_v3`：合并 main 与 trend_v2 各自最优 + 用户/分析提出的「趋势确认门」，**核心是用 dev-accel 门去解锁被 main 砍掉的高价入场带（0.68-0.79）**，验证能否把那部分负 EV 成交量救成正 EV，从而提高赚钱量。**纯 shadow，零影响 main 主策略与 trend_v2**（独立 ExperimentEngine、独立持仓/余额/jsonl）。

## 动机：candidates 回测把「dev-accel 门」从假设变成有证据

对 main 全部 58 笔 BTC momentum 成交，从 `main_candidates.jsonl`（236k 行，逐 tick 记 dev）重建入场前 ~90s 的 dev 轨迹（按 coin=BTC + ts 落在 entry_time 前 90s 匹配，匹配已逐笔验证：最后 tick dev ≈ 记录入场 dev、mr 单调）。52/58 匹配成功，结果：

| 切片 | n | 胜率 | EV/笔 | 净 |
|---|---|---|---|---|
| dev 上升（accel） | 35 | **62.9%** | +$0.020 | +$0.71 |
| dev 不上升 | 17 | 35.3% | −$0.023 | −$0.40 |
| **E≥0.70 全部** | 38 | 50.0% | −$0.009 | −$0.34 |
| **E≥0.70 & accel** | 22 | **63.6%** | **+$0.009** | **+$0.21** ✅ 转正 |
| E≥0.70 & not-accel | 16 | 31.2% | −$0.034 | −$0.55 |

**门把高价死带（50%/−EV）劈成「会跟随的 64%/+EV」和「追刀的 31%」。** 有用的是 ~90s 的 dev 趋势，不是 3-tick 微斜率（58% vs 56%，太噪声）。统计 z≈1.9（borderline），in-sample，n 偏小 → 需 shadow 出样本确认。

## trend_v3 设计（merge 三方最优）

| 维度 | 设计 | 来源 |
|---|---|---|
| 币种 | 仅 BTC | edge 只在 BTC（main+trend_v2 一致） |
| 时间窗 | mr 40-45 | main(35-44)/trend_v2(41-45) 交集 |
| 方向确认 | 1-tick 同向 | 承自 trend_v2 |
| dev 阈值 | ≥0.20% | 承自 main（回测口径） |
| **分层入场带** | 0.64-0.67 免门；**0.68-0.79 须过 dev-accel 门** | 回测：门控高价转正 |
| **dev-accel 门** | ~90s 内 abs_dev 上升 >0.01（deque 维护历史） | 本步回测核心 |
| 仓位 | $1.00 平 | 不引入 size 混淆，便于对照 |
| TP/Exit | 复用 trend_v2（`compute_trend_follow_tp_levels` / `evaluate_trend_follow_exit`） | trend_v2 出场更干净（stop_price 10% vs main 24%） |
| 运行 | 独立 ExperimentEngine，纯 shadow | 零风险 |

落盘字段 `entry_price_bucket` 区分 `cheap_0.64_0.67` / `high_gated_0.68_0.79`，`confidence_components` 记 `dev_rising`/`dev_flat`/`accel_warmup`，便于后续分析门控高价单是否复现 64%/+EV。

## 文件清单（全部加法，无删改 main / trend_v2 逻辑）

| 操作 | 文件 | 内容 |
|------|------|------|
| 修改 | `src/core/experiment_engine.h` | 声明 `evaluate_trend_v3` + `tv3_dev_hist_` deque 成员 + `<deque>/<utility>` |
| 修改 | `src/core/experiment_engine.cpp` | 实现 `evaluate_trend_v3` + 3 dispatcher 分支（entry/exit/TP，exit+TP 复用 trend_follow 的） |
| 修改 | `src/main.cpp` | 加 `trend_v3_experiment` 实例 + 3 API 回调 + 2 reset + 1 dispatch（`on_market`） |
| 修改 | `src/net/api_server.{h,cpp}` | `/api/experiment-trend-v3/{status,trades,all-trades}` 路由 + 回调 |
| 修改 | `src/dashboard.h` | "Trend Follow v3" tab + 面板 + fetch + render（复用 `renderExperiment`） |
| 新增（运行时） | `logs/experiment_trend_v3_trades.jsonl` + `_candidates.jsonl` | 自动创建 |

## 设计决策

- **dev-accel 门只卡高价（>0.67），便宜带免门**：回测显示便宜单基本自带 dev 上升、且便宜带 edge 已独立验证；门的价值在「让高价单变安全」。免门保住便宜带成交量，避免门的 warmup 误杀。
- **门用 ~90s wall-clock 窗口而非现成的 3-tick `expanding` 原语**：回测证明 3-tick 微斜率（~15-50s）分离弱（58/56），~90s 趋势才分离强（63/35）。用 `now_ms`（dispatcher 作用域内现成）维护 deque，cadence 无关。
- **复用 trend_v2 的 exit/TP 而非新写**：trend_v2 出场 profile 更干净，且减少新代码面。
- **$1.00 平仓、不抄 trend_v2 的 DOWN $1.25**：避免 size 非对称混淆「门是否有效」的判读。
- **纯 shadow、独立 engine**：main 的 `Strategy` 类与 `trades.jsonl`、trend_v2 的 `evaluate_trend_follow` 与其 jsonl 一字未动。trend_v3 只是 dispatch 处多调一次 `on_market`。

## 红线遵守

- [x] 没有引入硬编码密钥
- [x] 没有跳过错误处理
- [x] 编译通过、链接成功，无新 warning（既有 api_server L137/191 lint 与本步无关）
- [x] 二进制已 grep 确认含 trend_v3 逻辑（`high_gated_0.68_0.79`/`dev_rising`/`accel_warmup`）+ 3 路由 + dashboard tab

## 诚实声明（不吹）

- **门的证据是 in-sample、borderline（z≈1.9）、n 偏小（解锁那半 n=22）**。比 0.84 扩宽可信（真实成交、机制事先指定、是对真单过滤），但**不是确定结论**——shadow 出样本才算数。我对「trend_v3 能跑赢」的把握约 65-70%，不是 90%。
- 成功 = 门控的 `high_gated_0.68_0.79` 桶实测复现 ~64%/+EV；失败 = 门不够强 → 零损失，结论「便宜带已近最优」。

## 验收结果

```
[ 61%] Building CXX object .../main.cpp.o
[ 63%] Building CXX object .../core/experiment_engine.cpp.o
[ 65%] Building CXX object .../net/api_server.cpp.o
[ 68%] Linking CXX executable polymarket-arb
[100%] Built target polymarket-arb
```

## 部署计划（等 review 后）

按 SOP（[[server-access-deploy-ops]]）。**注意 dashboard 分工**：API 路由在 bot 二进制（要重编 bot），dashboard HTML 在 `dashboard.h`（manager 运行时读取，需 manager 重启）。所以这次两步都要：
1. 备份 jsonl + scp 改动的 6 个文件（experiment_engine.{h,cpp}、main.cpp、api_server.{h,cpp}、dashboard.h）。
2. `cmake -DPOLY_GIT_SHA=<sha>-dep<date>` 重配 → 停 bot → `-j2` 重编。
3. `systemctl restart polymarket-manager.service`（让 manager 重读新 dashboard.h）→ `POST /api/start` 起 bot。
4. 验证：`/api/experiment-trend-v3/status` 返回 JSON、`logs/experiment_trend_v3_candidates.jsonl` 30s 内出现并增长、dashboard 出现 "Trend Follow v3" tab。

## 后续 / 成功判据

1. **前向验证**：攒 ~50 笔后对比 trend_v3 vs main vs trend_v2 的 EV/笔 + 胜率；重点看 `entry_price_bucket=high_gated_0.68_0.79` 桶是否复现 ~64%/+EV（门解锁高价带成立的判据）。
2. 若门控高价单实测仍负 → 门不够强，可加强（更长窗口 / dev 绝对水平上限防"过度延伸"，见回测里 E=0.76 dev 峰 0.63 滚顶亏损那例）。
3. 若成立 → 考虑把门 + 解锁带提升进 trend_v2 或 main（届时另开 step + 实盘风控复核）。
