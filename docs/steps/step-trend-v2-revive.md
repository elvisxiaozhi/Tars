# Step — 复活 trend_follow 作为第 6 个 shadow 实验（trend_v2）

## 概要

把 `evaluate_trend_follow` 重新加回 main.cpp 作为独立 `ExperimentEngine` 实例。基于对历史 `experiment_trend_trades.jsonl`（n=148）的 regime 切片分析，确认 R2 段（2026-05-07 ~ 05-08）的 −$4.17 损失源于过松的旧参数配置（mr 无上限 / dev≥0.12 / 含 SOL+XRP+DOGE）——这些漏洞在当前 code 已全部封堵。反事实回放（排除 R2 + 仅 BTC/ETH/BNB）= +$1.44 / 77 笔 / wr 78%，年化外推 +$70/年。

## 关键命令

```bash
# 编译
cmake --build build -j

# 验证 trend_v2 已实例化（应输出 1 行）
grep -n "trend_v2_experiment" src/main.cpp | wc -l   # 期望 6（声明 + 3 API + reset_candle + reset_global_hour + on_market）

# 运行后看新策略入场（confidence ≥ 4 时才入）
./build/polymarket-arb 2>&1 | grep -E "SIGNAL.*trend_follow|trend_follow.*reject"

# 看新 jsonl 累积
ls -la logs/experiment_trend_v2_trades.jsonl
```

## 文件清单

| 操作 | 文件 |
|------|------|
| 修改 | `src/main.cpp`（5 处：实例化 / API 注册 / reset_candle / reset_global_hour / on_market dispatch） |
| 修改 | `src/net/api_server.h`（3 个方法声明） |
| 修改 | `src/net/api_server.cpp`（3 callbacks + 3 路由 + 3 setters） |
| 修改 | `src/dashboard.h`（6 处：HTML 面板 / persistDetails / destructure / fetchJSON × 3 / renderExperiment / renderCoinPnl） |
| 修改 | `docs/strategy-experiments.md`（§二 总表 +1 行 + 新增 §2.6 / §三 分发表 / §四 退役清单 / §七 dashboard scope） |
| 新增 | `docs/steps/step-trend-v2-revive.md`（本报告） |

## 设计决策

### 命名：为什么叫 `trend_v2_experiment` 而不是直接覆盖 `trend_experiment`

`trend_experiment` 变量在历史上跑过两次不同策略（先 `trend_follow`，后 `eth_late_cheap_v1`），是个**历史包袱命名**。我没改它（不破坏现有数据流），而是新增 `trend_v2_experiment` —— 表示"trend_follow 的第二轮实验"。日志文件 `experiment_trend_v2_trades.jsonl` 与历史 `experiment_trend_trades.jsonl` 不冲突。

### strategy_name 直接复用 `"trend_follow"`

`experiment_engine.cpp` L1962 的 dispatcher 已识别 `strategy_name_ == "trend_follow"`，不需要新加分支。直接复用代码路径，最小化改动。

### URL 前缀用 `/api/experiment-trend-v2`

避免与 `/api/experiment-trend`（实际是 `eth_late_cheap_v1`）冲突，前端可清晰区分。

### Dashboard：直接加新 `<details>` 面板而非用通用 scope 切换

当前 dashboard 的 5 个实验是 5 个独立面板（不是 scope 切换）。trend_v2 沿用同模式，避免重构整个 dashboard 数据流。

### 不修改 `evaluate_trend_follow` 代码本身

历史数据分析显示当前 code（c269567 + 090821f + fc4b47f + 8c5fb9a 累积后状态）的过滤已经够严。本次只复活实例化，不动入场/退出/TP 逻辑。等新 jsonl 累 50+ 笔后再回放看是否需调参（参考 [`step-doc-strategy-btc-1h-rewrite.md`](./step-doc-strategy-btc-1h-rewrite.md) 工作流）。

## 红线遵守

- [x] 没有引入硬编码密钥
- [x] 没有跳过错误处理
- [x] 编译通过，无 new warning（api_server.cpp 149/206 行的 `[bugprone-unused-return-value]` 是预存在的）
- [x] 未修改主策略 / live 下单 / 三层账户架构 / V2 字段 / 单仓制 / live 启动守卫
- [x] 仅 dry-run 模拟逻辑增加，trend_v2 与所有其它实验一样不会下真单（live 模式下也只跑 BTC 一仓）

## 验收结果

```
$ cmake --build build -j
[ 18%] Built target polymarket_crypto
[ 31%] Built target spdlog
[ 38%] Built target test_crypto
[ 43%] Built target import_key
[ 50%] Built target test_eip712
[ 59%] Built target test_chain
[ 61%] Building CXX object CMakeFiles/polymarket-arb.dir/src/main.cpp.o
[ 63%] Building CXX object CMakeFiles/polymarket-arb.dir/src/net/api_server.cpp.o
[ 65%] Linking CXX executable polymarket-arb
[100%] Built target polymarket-arb
```

## 预期影响

| 指标 | 当前 | 加 trend_v2 后预期 |
|---|---|---|
| ExperimentEngine 实例数 | 5 | 6 |
| Crypto 1h 市场 on_market 调用次数 | 2 / tick / coin | 3 / tick / coin |
| 每币种内存 / CPU | — | 小幅上升（+1 套 strategies_ map），单实例约 +几 KB |
| jsonl 文件 | 5 个活跃 | 6 个活跃（新增 `experiment_trend_v2_trades.jsonl`） |
| Dashboard | 5 个 `<details>` 面板 | 6 个 |
| API 端点 | `/api/experiment-trend(-finance/-crypto-4h/-crypto-daily)/...` | + `/api/experiment-trend-v2/...` |

历史回放预测（参考 `prompts/analyze_strategy.md` 反事实回放）：
- 笔频：trend_follow 入场窗 mr ∈ [41, 45] 仅 5 分钟，BTC/ETH/BNB 3 币，每 candle 最多 3 笔候选，confidence ≥ 4 后实际入场约 5-10 笔/天
- 净 PnL：基于历史 77 笔修正样本外推，约 +$0.18/天 / +$70/年（dry-run 规模）

## 遗留问题（下一步候选）

1. **观察 1-2 周新数据**：等 `experiment_trend_v2_trades.jsonl` 累积 30-50 笔后再做正式 v2 prompt 分析，对比历史数据预测
2. **建议 2**（给 `crypto_4h_updown_v1` / `crypto_daily_updown_v1` 加 direction_confirmed + 简易 confidence 门）— 等 trend_v2 表现验证后再做
3. **建议 3**（TP0 50% → 70%，加大 first take）— 等 trend_v2 出 20+ 笔后做敏感性分析
4. **trend_follow 同 candle 多个 market 怎么处理**？当前代码 trend_v2 与 `experiment` / `trend_experiment` 共享 `on_market` 调用，三个 engine 各自独立维护持仓（互不影响），但**同一币种、同 candle、三个 engine 都可能各开一仓**——这是预期行为（影子策略对比），不是 bug
