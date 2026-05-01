# 关键决策记录（ADR 风格）

只记**回头看仍要解释的决策**——参数调整（5min→8min, +25¢→+15¢ 等）放 git log 里，本文不抄。
每条结构：**背景 → 决策 → 为什么不选 X → 后果**。
按主题分组，组内大致按发生时间排。

---

## 架构层

### A1. LiveTrader 做成"门面类"，集中所有真金白银副作用
- **背景**：实盘改造 R1 起步时（commit `be5bc91`），需要决定真发订单的代码分布在哪。
- **决策**：单一 `LiveTrader` 类，封装钱包、链上读、CLOB 鉴权、下单、对账、应急平仓。`main.cpp` 持 `unique_ptr<LiveTrader>`，仅 `mode=="live"` 时构造；`dry_run` 模式 `live_trader == nullptr`，所有真发订单调用包在 `if (live_trader)` 内。
- **为什么不分散到各 core 模块**：副作用扩散到 `strategy / risk_manager / market_feed` 后，paper 和 live 路径会互相污染，dry_run 测试再也不能保证"不发单"。门面让 dry/live 边界明确，回滚也只需禁一个开关。
- **后果**：所有 V2 升级、V1→V2 字段迁移都只动一个类；`strategy.cpp` 和 `risk_manager.cpp` 实盘改造期一行未改。

### A2. 主循环用 REST polling，不用 WebSocket
- **背景**：早期实验过 `ws_client` 订阅 CLOB 价格流，但没接入策略主循环。
- **决策**：保留 `ws_client.{h,cpp}` 不删，但策略主循环用 `market_feed.refresh_order_book()` 同步 REST。Tick 间隔 5/10/15s 自适应（见 ARCHITECTURE §3）。
- **为什么不上 WS**：（1）单仓制 + 单一 BTC 1h 市场，对延迟不敏感（5s 粒度足够）；（2）WS 增量订单簿需要状态机维护一致性，事故面比 REST 大得多；（3）WS 断线 / 重订阅 / fill 错过的对账逻辑成本高，本项目人手不够。
- **后果**：tick 内最多 1 个 HTTP 抢断点，`g_running` 检查粒度 = `sleep_for(1s)` × N，SIGINT 1 秒响应；代价是高频策略不可行（不是当前目标）。

### A3. main 单线程跑业务，仅另起 1 个 api_server 线程
- **背景**：见 ARCHITECTURE §1。
- **决策**：所有业务（fetch / strategy / 下单 / journal）串行在 main 跑，`api_server` 在自己线程上服务 dashboard，靠 `SharedState` + `std::mutex` 同步。
- **为什么不开多线程**：单仓制场景没有并发下单；多线程的 race 复杂度远大于收益。
- **后果**：策略循环里**任何一个同步 HTTP 调用挂死都会卡住整个 bot**——这是 D2 的伏笔。

### A4. SIGINT 应急平仓做成两阶段：先 cancel 再 SELL FOK
- **背景**：commit `22943ca`，R-V2.6 接主循环时发现单纯 `SELL FOK` 有风险——bot tracked positions 是"已 fill 的仓"，但**还有未 fill 的 BUY 订单挂着**，对那些订单发 SELL 会让 server 报错或反向下单。
- **决策**：`emergency_close_all` 第一步 `reconcile_on_startup` + 逐个 `cancel_order`，第二步对 bot 已 track 的 positions 走 SELL FOK @ 0.01。
- **后果**：SIGINT 退出干净，无残留挂单。代价：emergency 路径多一个网络往返，~2-3s。

---

## 实盘下单层（V2 升级踩坑驱动）

### B1. V1 → V2 整体迁移（一刀切，不并存）
- **背景**：2026-04-28 Polymarket 升级 CTFExchange V2，PolyOrder 结构变更（删 `taker/expiration/nonce/feeRateBps`，加 `signer/timestamp/metadata/builder`），余额从链上 ERC-20 改成 ledger（`/balance-allowance`）。
- **决策**：commit `b7010d7` 起一系列 R-V2.x，把 `PolyOrder` 结构、签名、序列化、余额读取**全切到 V2**，不保留 V1 路径。R6 (V1 approval check) 直接废弃（commit `43846cf` 删了 stale warning）。
- **为什么不双轨**：V2 是合约级强制升级，没有"V1 仍可用"的理由；保留 V1 代码只是噪音。
- **后果**：当前不能与未升级的旧合约/旧账户交互——用户必须在 polymarket.com 走完 V2 onboarding。

### B2. R-V2.7 P0：BUY 后必须轮询 `/data/order/<id>` 直到 fill
- **背景**：commit `8a2f53e`。第一次实盘 P3 case：BUY limit GTC 假设立即 fill，bot 创建了 paper position，结果挂单 38 分钟才 fill；期间价格暴跌，触发 -50% 止损。SELL 时 server 报 "not enough balance"，重试 10 次浪费 5 min。
- **决策**：`place_entry_order` 返回后，主循环轮询 `get_order(buy_id)` max 60s / 每 3s 一次；只有 `status=="matched"` 才创建 paper position，否则 `cancel_order` 跳过本信号。
- **为什么不假设 GTC 总能 fill**：实测就是会挂；策略 paper state 与 server ledger 一旦偏离就连锁错算。
- **后果**：每个信号最多多等 60s；不会 fill 的信号自动放弃，避免 P3 类亏损。

### B3. R-V2.7 P1：FOK SELL 的 limit 价用 floor `$0.01`
- **背景**：commit `8a2f53e` 同次。出场时 strategy 给的 `exit_price` 可能高于 best_bid，FOK 找不到 ≥ price 的全量买家就 kill，仓位卡死。
- **决策**：LIVE FOK SELL 永远用 `price=0.01` floor；server 自动按对手最佳 bid 全成交。
- **为什么不动态算价**：FOK 不会"滑价成交"，要么全成要么不成，floor 是最稳的"接受任何 bid"信号。
- **后果**：paper P&L 仍按 strategy 价记，LIVE 实际成交价可能稍低（自然差异）——这导致 B4。

### B4. 用真实成交价覆盖逻辑价（B3 的副作用兜底）
- **背景**：commit `ca0648b`。一笔 trailing_stop：日志记 +$0.25，真实余额 +$0.59，差 5-10¢。原因是 `POST /order` 响应里的 `makingAmount/takingAmount` 之前没解析。
- **决策**：`send_v2_order` 解析两个字段算出 `filled_shares + filled_avg_price` 写入 `OrderResult`；主循环入场后用真实价覆盖 `pos.entry_price/shares/size_usdc`，重算 `tp_levels` 和 MFE 基线。
- **为什么不只用 paper 价**：paper 与真实余额持续偏离 5-10¢，几小时后 dashboard 显示的 P&L 和实际钱包余额对不上，无法对账。
- **后果**：策略 P&L 与 server ledger 严密同步；响应缺字段时退回逻辑价 + warning（B3/B4 不互锁）。

### B5. LIVE 下单失败 → 不更新 paper state
- **背景**：commit `113bd84` R-V2.6。
- **决策**：入场 `place_entry_order` 失败 → `continue` 不创建 paper position；TP/止损 `place_exit_order` 失败 → 不标 closed，下个 tick 重试。
- **后果**：paper state 永远是 server 的子集，最坏情况是"server 已成交、paper 还显示开仓"——下次 tick 再发 SELL，server 拒（"already closed"），paper 才修正。可观察、可恢复。

---

## 网络与可靠性

### D1. 所有网络模块必须支持 HTTP/HTTPS 代理（CONNECT 隧道）
- **背景**：开发环境在大陆，需要走本地代理（`127.0.0.1:7897`）。
- **决策**：`HttpClient`、`WsClient`、`ChainClient` 构造函数都接受 `proxy_url`；`AppConfig.network.proxy_url` 留空时自动读环境变量。
- **后果**：所有外网交互单点配置；新增网络模块必须沿用此约定（写在 CLAUDE.md 硬约束里）。

### D2. HTTP 客户端加 wall-clock 兜底（35s 硬上限）
- **背景**：commit `28a7963`，2026-05-01 实测：BUY 调用挂了 28 分钟，远超配的 15s timeout。`boost::beast` 的 `expires_after()` 在某些代理/SSL 异常路径下未生效。
- **决策**：`HttpClient` 内部用 `packaged_task + detach 线程 + future.wait_for(35s)` 包一层。Boost 自己的 deadline 仍优先生效，35s 仅最后防线。`io_context` 从 Impl 成员改为栈局部（detach 多线程要求）。
- **为什么不只调 boost timeout**：实测就是不可靠；wall-clock 兜底是已知行为可控的最后防线。
- **后果**：单次 HTTP 最长 35s，不会再卡死整个 strategy loop（A3 单线程的关键修复）。

### D3. 兜底路径吞 `cancel_order` 和 `read_polymarket_balance` 异常
- **背景**：emergency_close 时如果 `cancel_order` 抛出来，整个退出流程会断；fill 后刷余额失败也不该让交易失败。
- **决策**：`try { ... } catch (...) {}`（`main.cpp` L968, L1002, L1131, L1220, L1381）。
- **为什么不传播**：这些都是"已经在兜底路径"的辅助调用，再抛只会让坏情况更坏。业务路径（fetch / strategy / 下单）的异常仍正常 catch + log。
- **后果**：兜底路径稳健；代价是这些异常不进 `spdlog::error`——靠业务路径下一次 tick 自然修正。

---

## 策略与风控

### S1. 单仓制 + 本场 K 线锁
- **背景**：BTC 1h 市场每小时只有一个有效合约，且 dev 信号的"甜区/苦区"在一根 K 线内大致一致。
- **决策**：`risk_manager` 限制 `open_positions ≤ 1`；触发止损后设 `candle_stopped_=true`，本场 K 线禁止再开仓；新 K 线（市场 slug 变化）自动 `reset_candle()`。
- **为什么不允许多仓**：（1）账户余额小（~$20），多仓同方向放大单点风险；（2）本场亏了再追加只会"接刀"——历史数据反复印证。
- **后果**：信号被风控拒绝是正常状态——`reject_aggregator` 每 10 次或 5 分钟 flush 汇总日志，避免日志噪音（commit `2623845`）。

### S2. 入场固定 5 shares，不按 Kelly / 比例
- **背景**：早期试过 17 shares（`924bc26`）和 10 shares（`5e1e8cb`），最终回到固定 5 shares。
- **决策**：`risk_manager.fixed_shares_ = 5`，TP 切成 2/2/1 shares。
- **为什么固定不动态**：（1）账户太小，Kelly 系数估计误差远大于本身；（2）fee 模型有 `shares × rate × p × (1-p)` 项，maker 0% 但 taker 7.2%，整数 shares 有利于精确 TP 拆分。
- **后果**：每笔成本可预测；当前规模下不优化 sizing，先优化 win rate。

### S3. 入场必须用 limit @ ask-1¢（maker，免 fee）
- **背景**：`strategy.evaluate_entry` 返回 `entry_price = market_ask - 0.01`，`is_taker=false`。
- **决策**：入场永远不吃 ask（taker 7.2% 会吃掉所有边际利润）；挂在 ask 下方等对手吃。
- **为什么不直接吃**：`min_net_profit_pct` 阈值在 taker 费下基本永远 < 0；maker 路径才有利可图。
- **后果**：入场可能 fill 不上（B2 的根本原因）；用 60s 轮询 + cancel 兜底。

### S4. dry_run 关掉 kill_switch / consecutive_loss / daily_drawdown
- **背景**：commit `9abb37c`。测试期需要让所有信号都进入 paper trading 链，否则止损后剩半天没数据收。
- **决策**：dry_run 模式下注释掉 `record_loss` 触发的 kill switch；live 模式重新启用（TODO 写在策略文档里）。
- **后果**：dry_run 数据完整；切 live 前**必须**回滚此项（已在 RUNBOOK 提示）。

---

## 数据与可观测性

### O1. TradeJournal 启动加载历史 jsonl + session_start_index 区分本会话
- **背景**：commit `57cafb9` + `43846cf`。
- **决策**：每次启动加载完整 `logs/trades.jsonl` 进 `records_`，记录 `session_start_index_`。`print_summary` 退出时只详细打印本会话新增；all-time totals 按 `mode` 分组成 dry / live 两行。
- **为什么不分两个文件**：单文件历史完整，dashboard 可直接渲染所有交易；mode 字段足以分组。
- **后果**：退出日志从 ~115 行降到 5-10 行；老记录加载时 `mode` 缺省值 = `"dry_run"`（commit `1ede3d0` 提供 `patch_live_mode.py` 离线修历史标签）。

### O2. Dashboard 由 `mode` filter 切换数据源（live / dry / all）
- **背景**：commit `15d9779` + `1ede3d0` + `40edc23`。
- **决策**：后端 `/api/analytics` 顶层=全部记录、`by_mode.{dry_run,live}` 分组；前端 filter 按钮切数据源 + 持久化到 localStorage。
- **后果**：一份代码、一个数据源、三种视图；live 数据少时不会被 dry 历史淹没。

### O3. MFE/MAE/dead_water 埋点是观测优先，不立刻改决策
- **背景**：commit `8125580`、`32a43f3`、`3469a01`。
- **决策**：先在 `Position` / `TradeRecord` 加观测字段（`mfe_at_5/10/15min`、`mfe5_gain_pct`、`dw_btc_dev_pct` 等），跑 N 笔后再回归是否要改阈值。
- **为什么不直接调阈值**：4 笔数据的回归会过拟合；commit messages 里写明 "20+ 笔后再评估"。
- **后果**：策略调参靠数据驱动而非直觉；代价是新埋点要等数据攒够才启用。

### O4. 移除"BTC dev 0.15%-0.50% 甜区"过滤——回测过拟合的教训
- **背景**：commit `924bc26` 加了甜区过滤（38 笔回测得出），commit `b787586` 又删除——4-28 实盘 P3/P4 两笔大赢家在低 dev 区，会被甜区误杀。
- **决策**：删除区间过滤，恢复全 dev 范围入场。
- **后果**：少量样本回测得出的"甜区"不可信——这是项目历史最大的方法论教训，记一笔提醒后人。

---

## 工作流

### W1. 每一步：代码 → step 报告 → user review → commit → 更新 memory → 下一步
- **背景**：项目长链路（R1-R10、Step 2.1-2.24）需要可追溯的迭代。
- **决策**：每个 step 落 `docs/steps/step-<N>-report.md`（模板在 `TEMPLATE.md`），等 user review 通过才 commit。
- **后果**：36 份 step 报告 + git log 双索引，新人可按时间线复盘任何决策。
