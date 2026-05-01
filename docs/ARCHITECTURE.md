# 架构 — 线程、数据流、Tick 生命周期

读完本文你应该能回答：**程序里有几个线程？谁写谁读什么数据？一个 tick 内发生了什么？**
入口和模块清单见 `CLAUDE.md`，本文专讲"运行时长什么样"。

---

## 1. 线程拓扑（只有 2 个长生命周期线程）

```
┌─────────────────────────────────────────────────────────────────┐
│  main thread                          api_server thread          │
│  (策略主循环, while g_running)         (Boost.Beast HTTP server)  │
│                                                                   │
│   binance.fetch()                     accept connections          │
│   market_feed.fetch_markets()         dispatch /api/status        │
│   strategy.evaluate_*()                       /api/trades         │
│   live_trader.place_*()                       /api/stats          │
│   journal.record()                            /api/analytics      │
│   ↓ writes                                    /                   │
│        ┌──────────────────────────────────┐                       │
│        │   SharedState (main.cpp L152)    │ ←── reads ───┐        │
│        │   - mu : std::mutex              │              │        │
│        │   - btc : BtcMarketData          │              │        │
│        │   - positions : vector<Position> │              │        │
│        │   - tick_count, daily_pnl, etc.  │              │        │
│        └──────────────────────────────────┘              │        │
│                  ↑ writer (lock_guard)            reader (callbacks)│
└─────────────────────────────────────────────────────────────────┘
```

**仅此而已**——没有独立的 market-feed 线程、WS 线程、订单管理线程。所有业务逻辑都在 main thread 串行执行，靠 poll 间隔节流。

短生命周期线程：`live_trader.place_*` 内部 `HttpClient` 自带的 io_context 线程（请求结束即销毁）。

### 为什么不开更多线程？
- Polymarket 的 BTC 1h 市场只有一个，单仓制 → 没有并发下单需求
- WS 流当前没用（早期 R 步骤实验过，最终选 REST polling 简化对账）
- 所有副作用集中在 main 一条链上，bug 容易定位、SIGINT 退出干净

---

## 2. SharedState — 唯一的跨线程共享数据

定义在 `src/main.cpp` L152-162。**只有 5 个写入点 + 4 个读取点**，全部 `lock_guard`：

| 位置 | 操作 | 字段 |
|---|---|---|
| L820（每 tick 顶部） | 写 | `btc`, `tick_count`, `consecutive_losses`, `daily_pnl` |
| L1308（tick 尾） | 写 | `positions`（清理 closed 后整体覆盖） |
| L1319（决定 sleep_sec） | 读 | `btc.minutes_remaining` |
| L315 / 415（API 回调） | 读 | 全部字段，序列化成 JSON |

API 回调函数是 `std::function`，由 `api_server` 线程调用——所以 callback 体内必须先拿锁再读。

---

## 3. 一个 Tick 的生命周期（main 主循环）

`main.cpp` L811 起 `while (g_running)`。每个 tick 的步骤：

```
┌─ 0. (LIVE only) refresh_balance_if_stale(5min) ──┐ L814
│
├─ 1. binance.fetch()  ─────────────────────────────┤ L816
│      → BTC 现价 + 1h K线 + 24h 平均波动率
│
├─ 2. 写 SharedState.btc + tick_count ──────────────┤ L820
│
├─ 3. market_feed.fetch_markets()  ─────────────────┤ L827
│      → gamma API，按 market_filter="btc-updown" 过滤
│      → 通常只拿当前小时的那 1 个市场
│
├─ 4. 检测 K线切换（slug 变了）──────────────────────┤ L836
│      → 新 candle: risk.reset_candle()
│
├─ 5. for each market:                              │ L848
│   ├─ refresh_order_book(cid)  → CLOB REST         │ L851
│   ├─ extract_quotes() → up_ask / down_ask         │ L855
│   ├─ strategy.evaluate_entry() → EntrySignal      │ L866
│   ├─ if sig.valid && risk.can_open_position():    │ L872
│   │     ├─ (LIVE) live_trader.place_entry_order() │ L934
│   │     ├─ (LIVE) poll get_order(id) max 60s/3s   │ L952
│   │     ├─ (LIVE) cancel if not filled            │ L968
│   │     ├─ (LIVE) refresh balance                 │ L1002
│   │     └─ positions.push_back(pos)               │ L1005
│   │
│   └─ for each open position:                      │
│        ├─ strategy.evaluate_exit()                │
│        ├─ if exit:                                │
│        │   ├─ (LIVE) live_trader.place_exit_order│ L1114/1204
│        │   ├─ (LIVE) refresh balance              │
│        │   ├─ journal.record()                    │ L1295
│        │   └─ pos.closed = true                   │
│        └─ update MFE/MAE/dead_water 埋点           │
│
├─ 6. erase closed positions ───────────────────────┤ L1300
├─ 7. 写 SharedState.positions ─────────────────────┤ L1308
├─ 8. 算 sleep_sec（5 / 10 / 15 秒, 见下）           │ L1317
├─ 9. flush reject_aggregator（≥10 次或 5 分钟）     │ L1331
└─ 10. 打印紧凑状态行 + sleep                         │ L1346
```

### Tick 频率自适应（L1321-1327）
| 状态 | 间隔 |
|---|---|
| 末 10 分钟（`mins ≤ 10`） | 5s |
| 有持仓 | 10s |
| 空闲 | 15s |

加上 SIGINT 是 1 秒粒度（`sleep_sec` 拆成多段 `sleep_for(1s)`，循环里检查 `g_running`），所以 Ctrl-C 至少 1 秒响应。

---

## 4. 数据流（外部 → 内部）

```
                         ┌──────────────────┐
        Binance API ───→ │  binance_feed    │ → BtcMarketData → state.btc
                         └──────────────────┘
                                                       ↓
                         ┌──────────────────┐    ┌──────────┐
   gamma + CLOB REST ──→ │  market_feed     │ →  │ strategy │
                         └──────────────────┘    └──────────┘
                                                       ↓
                                              EntrySignal / ExitSignal
                                                       ↓
                         ┌──────────────────┐    ┌─────────────┐
                         │  risk_manager    │ ←→ │ position 数组 │
                         └──────────────────┘    └─────────────┘
                                                       ↓ (LIVE)
                  ┌────────────────────────────────────┐
                  │            live_trader              │
                  │   ├─ chain_client → Polygon RPC    │
                  │   ├─ http_client  → CLOB REST      │
                  │   └─ wallet/eip712 → sign          │
                  └────────────────────────────────────┘
                                                       ↓
                         ┌──────────────────┐
                         │  trade_journal   │ → logs/trades.jsonl
                         └──────────────────┘
                                                       ↓
                         ┌──────────────────┐    ┌──────────────┐
                         │  SharedState     │ ←─ │ api_server   │ → dashboard.h (HTML)
                         └──────────────────┘    └──────────────┘
```

**关键约束**：`live_trader` 是**唯一**碰真金白银的类。`dry_run` 模式下 `live_trader` 是 `nullptr`，所有 `if (live_trader)` 分支跳过——paper trading 路径完全不变。

---

## 5. LiveTrader 内部子流程（仅 LIVE 模式）

### 启动守卫（`main.cpp` L189-228，渐进式自检）
```
mode == "live"
   ↓
init_wallet()                  R2  解密 keystore.enc + 验地址
   ↓
read_chain_state()             R3  Polygon eth_call 读 USDC/CTF/allowance
   ↓
ensure_clob_authenticated()    R5  EIP-712 签 → POST /auth/api-key 拿 L2 三件套
   ↓
read_polymarket_balance()    R-V2.3  GET /balance-allowance 读 pUSD ledger
   ↓
reconcile_on_startup()         R9-V2  GET /data/orders 列残单，提示 user 处理
   ↓
进入主循环
```
任一步抛异常 → fail-fast 退出 1。**不要绕过守卫**。

### 入场下单（`live_trader.place_entry_order`）
```
sig (EntrySignal)
   ↓
build PolyOrder V2 字段（含 signer/timestamp/metadata/builder）
   ↓
sign_poly_order()  → 65-byte ECDSA sig
   ↓
polyorder_to_json()  → V2 紧凑格式
   ↓
POST /order  with L2 HMAC headers
   ↓
parse OrderResult { order_id, filled_shares, filled_avg_price, ... }
```

### 出场（`place_exit_order`）
- TP 部分平仓：limit @ tp_price（maker，免 fee）
- 止损 / 应急：FOK market（taker，0.072 fee）

### SIGINT 应急平仓（`main.cpp` L1374）
循环退出后，若 `live_trader` 存在且仍有 `positions`：
```
emergency_close_all(positions)
   → for each: SELL FOK @ price=0.01
       (server 用对手最佳 bid 撮合，floor 价是为了"不挂回挂单簿")
```

---

## 6. 失败模式与兜底

| 场景 | 行为 |
|---|---|
| Binance API 超时 | catch → log → 下一轮重试（无停机） |
| gamma 返回空 | sleep 30s 后重试（L829-832） |
| CLOB orderbook 不全（< 2 token） | 跳过这个市场，下一个 |
| LIVE 下单超时 60s 未成交 | cancel + 跳过本信号（不创建 paper position） |
| `cancel_order` 失败 | 吞异常（已是兜底路径，不能再抛） |
| `read_polymarket_balance` 失败 | 吞异常（用上次缓存值）|
| SIGINT/SIGTERM | `g_running=false` → 主循环退出 → emergency_close_all → api.stop |
| 单次 HTTP 调用挂死 | http_client 内置 wall-clock guard（cc14612 commit）|

**所有 catch 都是 `catch (const std::exception&)` + log，不是吞所有 throw**——程序错误（assert / segfault）会立刻退出。

---

## 7. Dashboard 数据契约

`api_server` 暴露 4 个 endpoint，全是 `GET` 返 JSON：

| Path | 内容来源 | 主要字段 |
|---|---|---|
| `/api/status` | `state.btc` + `state.positions` + `display_balance()` | `mode`, `btc`, `positions`, `account_balance`, `tick_count` |
| `/api/trades` | `journal.records()` | `trades` 数组（含 mode, pnl, exit_reason 等） |
| `/api/stats` | `journal` 聚合 | `total_pnl`, `candle_win_rate`, `by_mode`（dry_run vs live 分计）|
| `/api/analytics` | `journal.records()` 计算 | MFE 分布、出场效率、连胜连亏统计等 |
| `/` | `set_dashboard_html(DASHBOARD_HTML)` | `dashboard.h` 编译期字符串 |

前端是单页 HTML/JS（无构建步骤、无外部 CDN），定时 `fetchJSON` 这 4 个端点刷新。

---

## 8. 关键文件 → 关键行号速查

```
main.cpp:152     SharedState 定义
main.cpp:166     main() 入口
main.cpp:189-228 LIVE 启动守卫链
main.cpp:312-776 4 个 API 回调注册
main.cpp:811     主循环 while
main.cpp:848     市场遍历
main.cpp:872     入场判断分叉
main.cpp:934     LIVE 下单
main.cpp:1114    LIVE TP 出场
main.cpp:1204    LIVE 止损出场
main.cpp:1300    清理 closed positions
main.cpp:1317    自适应 sleep
main.cpp:1374    emergency_close_all
```
