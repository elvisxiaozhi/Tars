# Polymarket BTC 1h 自动交易 Bot

C++17 / Boost.Beast / OpenSSL / spdlog / nlohmann-json，运行于 macOS。
**不是通用套利**——是针对 Polymarket BTC 1 小时二元期权（UP/DOWN）的单仓制定向策略。
当前阶段：实盘改造中，分支 `live-trading`，路线图 R1-R9，R1-R6 已落地，V2 合约升级（2026-04-28）已适配。

---

## 入口指引（按你想干嘛跳转）

| 想做的事 | 去哪看 |
|---|---|
| 改策略逻辑（入场/TP/止损/死水）| `src/core/strategy.{h,cpp}` |
| 改 / 加实验（shadow）策略 | `src/core/experiment_engine.{h,cpp}`；当前 5 个活跃实验见 `docs/strategy-experiments.md` |
| 分析策略日志（trades.jsonl / experiment_*.jsonl） | `prompts/analyze_strategy.md`（v2 标准化 prompt） |
| 改实盘下单/对账/链上读 | `src/core/live_trader.{h,cpp}` |
| 改风控（单仓制、本场锁、每日损） | `src/core/risk_manager.{h,cpp}` |
| 改主循环 / 线程编排 / 共享状态 | `src/main.cpp`（1389 行，`SharedState` 在 L152，`main()` 在 L166）|
| 改前端仪表板 | `src/dashboard.h`（内嵌 HTML/JS 字符串常量）|
| 改 EIP-712 / V2 订单字段 | `src/core/polymarket_types.{h,cpp}` + `src/crypto/eip712.{h,cpp}` |
| 看历史进度 / step 报告 | `docs/steps/`（36 份）+ `git log`（事实来源） |
| 看路线图当前位置 | `~/.claude/projects/.../memory/MEMORY.md` 中的 project_progress / project_live_trading |
| 想懂线程拓扑 / 数据流 / tick 生命周期 | `docs/ARCHITECTURE.md` |
| 想懂某个反直觉决策的"为什么" | `docs/DECISIONS.md`（21 条 ADR + 事故教训） |
| 切换 dry/live、排障、重置数据、应急平仓 | `docs/RUNBOOK.md` |
| 日志关键词 → 含义 | `docs/log_reference.md` |

`docs/design.md` 是**老设计**（提到 ArbDetector/OrderExecutor/WalletManager），代码已 pivot，仅作历史参考，不要按它写新代码。

---

## 模块地图

```
src/
├── main.cpp              主循环、线程编排、SharedState、SIGINT、状态聚合 → API
├── dashboard.h           内嵌 dashboard HTML（编译期字符串）
│
├── core/                 业务引擎（namespace polymarket）
│   ├── binance_feed      BTC 现价 + 1h K线 + 24h 平均波动率
│   ├── market_feed       Polymarket 市场列表（gamma）+ 订单簿（CLOB）
│   ├── strategy          BTC 1h 二元期权：入场判断、TP0/1/2、trailing、最后 10min
│   ├── risk_manager      单仓制 + 本场 K 线锁 + 连胜连亏 + 余额
│   ├── live_trader       实盘门面：钱包 / 链上读 / CLOB auth / V2 下单 / 对账 / 应急平仓
│   ├── trade_journal     JSONL 落盘 + 会话内摘要 + K 线级胜率
│   └── polymarket_types  V2 PolyOrder、ClobAuth、L2 HMAC、order JSON 序列化
│
├── crypto/               namespace polymarket（签名/钱包）
│   ├── wallet            Web3 keystore v3 + EIP-55 地址派生 + PrivateKey RAII
│   ├── eip712            通用 EIP-712 原语（typeHash/structHash/domain/sign）
│   ├── keccak256         Keccak-256
│   ├── hmac_sha256       L2 API 鉴权
│   └── base64            url-safe base64
│
├── net/                  namespace polymarket::net（全部支持本地代理）
│   ├── http_client       Boost.Beast HTTPS + CONNECT 隧道
│   ├── ws_client         WSS 异步消息循环 + 回调
│   ├── chain_client      Polygon JSON-RPC + ABI encode + 多 RPC failover
│   └── api_server        内嵌 HTTP server（dashboard 端口）
│
├── utils/
│   ├── config            JSON → AppConfig + 初始化 spdlog
│   ├── types             Market / Token / OrderBook / BestBidAsk
│   └── json_helpers      JSON 解析辅助
│
├── cli/                  ⚠ 空目录（design.md 规划过、未实现）
└── storage/              ⚠ 空目录（design.md 规划过、未实现）

tests/                    test_crypto / test_chain / test_eip712（KAT 验证）
tools/                    import_key（明文私钥 → keystore.enc）+ 几个 Python 辅助
third_party/              spdlog（submodule）+ json.hpp（header-only）
config/                   config.example.json（模板）+ config.json（实际，gitignored）
logs/                     bot.log + trades.jsonl（gitignored）
docs/steps/               每一步的实施报告（参考 workflow 规则）
```

---

## 硬约束（违反这些会出真实事故）

1. **必须走代理**：所有 HTTP / WS / RPC 都接受 `proxy_url`，默认 `127.0.0.1:7897`。新加网络模块必须支持 CONNECT 隧道。
2. **三层账户架构**（live 模式），不能混用：
   - `wallet.address`（EOA）—— 你的私钥派生地址，**只用来签 EIP-712**
   - `polymarket.proxy_address`（Simple7702Account）—— 持仓/订单 maker
   - `polymarket.api_address`（CLOB profile 里的 "API use only"）—— L2 HMAC 鉴权
3. **V2 字段已生效**（2026-04-28 起）：`PolyOrder` 含 `signer / timestamp / metadata / builder`，**不再有** `taker / expiration / nonce / feeRateBps`。
4. **V2 余额走 ledger**：用户资金不在 proxy 钱包链上，必须 `GET /balance-allowance` 读 pUSD（`live_trader.read_polymarket_balance`）。
5. **私钥不进日志**：`PrivateKey` 是 RAII 自动 `secure_zero`；任何打印路径只能输出地址。
6. **单仓制**：`risk_manager` 限制最多同时 1 仓；本场（本 1h K 线）止损后锁仓直到下一根 K 线。
7. **Live 模式启动守卫**（`main.cpp` L189-228）按 R2→R3→R5→R-V2.3 顺序自检，任一失败立即退出，不要绕过。

---

## 配置（关键字段）

`config/config.json`（gitignored，从 `config.example.json` 复制）。
| 字段 | 含义 |
|---|---|
| `strategy.mode` | `"dry_run"` 或 `"live"`——所有 LiveTrader 副作用都由这个开关 |
| `strategy.poll_interval_sec` | 主策略循环间隔 |
| `strategy.market_filter` | gamma API 过滤词，默认 `"btc-updown"` |
| `network.proxy_url` | 空 = 用环境变量；否则 `http://127.0.0.1:7897` |
| `network.api_port` | dashboard 端口（默认 9090） |
| `wallet.keystore_path` | `./keystore.enc`（用 `tools/import_key` 生成） |
| `polygon.rpc_urls` | 空 = 走代码内置 publicnode/drpc/1rpc failover |

---

## 编译 / 运行 / 调试

```bash
# 编译（CMake，第一次）
cmake -S . -B build && cmake --build build -j

# 跑（默认找 ./config/config.json）
./build/polymarket-arb

# 跑指定配置
./build/polymarket-arb /path/to/config.json

# 单元测试（KAT）
./build/test_crypto && ./build/test_chain && ./build/test_eip712

# 一次性：明文私钥 → keystore.enc
./build/import_key

# Dashboard
open http://127.0.0.1:9090
```

依赖：`brew install boost openssl@3 secp256k1 cmake`；spdlog 走 submodule。

---

## 工作流约定（与 memory 一致）

每一步：写代码 → 在 `docs/steps/step-<N>-report.md` 写报告 → 等 user review → `git commit` → 更新 memory → 进下一步。新步骤参考 `docs/steps/TEMPLATE.md`。
