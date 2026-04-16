# Polymarket 套利交易机器人 — 设计文档

## 1. 项目概述

基于 C++ 实现的 Polymarket 自动套利交易程序，运行于本地 macOS 环境，通过秒级监控发现套利机会并自动执行交易。

### 1.1 套利类型

Polymarket 上主要存在以下套利机会：

| 类型 | 原理 | 示例 |
|------|------|------|
| **互补事件套利** | 同一市场 Yes + No 价格之和 ≠ $1.00 | Yes=$0.52, No=$0.46, 两边买入锁定 $0.02 利润 |
| **多结果套利** | 多选项市场所有选项价格之和 ≠ $1.00 | A=$0.30, B=$0.25, C=$0.20, D=$0.15, 总和=$0.90 < $1.00 |
| **关联市场套利** | 逻辑关联的不同市场出现定价矛盾 | "X 当选总统" vs "X 赢得初选" 出现概率倒挂 |

### 1.2 核心指标

- 监控延迟：< 1 秒（WebSocket 推送）
- 套利判断 + 下单延迟：< 100ms
- 最小套利利润阈值：可配置（默认扣除 gas + 手续费后 > 0.5%）

---

## 2. 系统架构

```
┌─────────────────────────────────────────────────────┐
│                     CLI Interface                    │
│            (状态显示 / 命令输入 / 日志输出)            │
└──────────────────────┬──────────────────────────────┘
                       │
┌──────────────────────▼──────────────────────────────┐
│                    Core Engine                        │
│  ┌─────────────┐ ┌──────────────┐ ┌──────────────┐  │
│  │ MarketFeed  │ │ ArbDetector  │ │ OrderExecutor│  │
│  │ (数据采集)   │→│ (套利检测)    │→│ (订单执行)   │  │
│  └─────────────┘ └──────────────┘ └──────────────┘  │
│  ┌─────────────┐ ┌──────────────┐ ┌──────────────┐  │
│  │ RiskManager │ │ PositionMgr  │ │ WalletMgr    │  │
│  │ (风控)      │ │ (仓位管理)    │ │ (钱包/签名)  │  │
│  └─────────────┘ └──────────────┘ └──────────────┘  │
└──────────────────────┬──────────────────────────────┘
                       │
┌──────────────────────▼──────────────────────────────┐
│                  Infrastructure                      │
│  ┌──────────┐ ┌──────────┐ ┌───────┐ ┌───────────┐ │
│  │ Network  │ │ Storage  │ │ Logger│ │ Config    │ │
│  │ (HTTP/WS)│ │ (SQLite) │ │(spdlog)│ │ (YAML)   │ │
│  └──────────┘ └──────────┘ └───────┘ └───────────┘ │
└─────────────────────────────────────────────────────┘
```

---

## 3. 模块详细设计

### 3.1 MarketFeed — 数据采集

负责从 Polymarket CLOB API 获取实时市场数据。

**数据源：**

- **REST API** (`https://clob.polymarket.com`)
  - `GET /markets` — 获取所有活跃市场
  - `GET /book?token_id=XXX` — 获取订单簿
  - `GET /price?token_id=XXX&side=buy` — 获取当前最优价格
- **WebSocket** (`wss://ws-subscriptions-clob.polymarket.com/ws/market`)
  - 订阅订单簿变动，获取实时 bid/ask 更新

**核心数据结构：**

```cpp
struct TokenPrice {
    std::string token_id;
    double best_bid;        // 最高买价
    double best_ask;        // 最低卖价
    double bid_size;        // 买方深度
    double ask_size;        // 卖方深度
    uint64_t timestamp_ms;  // 更新时间戳
};

struct Market {
    std::string condition_id;
    std::string question;       // 市场问题描述
    std::vector<Token> tokens;  // Yes/No 或多选项
    bool active;
    double volume;
    double liquidity;
};

struct Token {
    std::string token_id;
    std::string outcome;    // "Yes" / "No" / 选项名
    TokenPrice price;
};
```

**工作流程：**

1. 启动时通过 REST 拉取所有活跃市场全量快照
2. 建立 WebSocket 连接，订阅关注市场的价格更新
3. 增量更新本地订单簿缓存
4. 定时（每 30s）REST 全量校验，防止 WebSocket 数据漂移

### 3.2 ArbDetector — 套利检测

每次价格更新时触发检测。

**互补事件套利检测：**

```cpp
struct ArbOpportunity {
    std::string market_id;
    ArbType type;               // COMPLEMENT / MULTI_OUTCOME / CROSS_MARKET
    std::vector<ArbLeg> legs;   // 套利的各条腿
    double gross_profit;        // 毛利润 (未扣费)
    double net_profit;          // 净利润 (扣除手续费+gas)
    double confidence;          // 置信度
    uint64_t detected_at;
};

struct ArbLeg {
    std::string token_id;
    Side side;          // BUY / SELL
    double price;
    double size;        // 可执行数量
};
```

**检测逻辑：**

```
互补事件套利:
  cost = best_ask(Yes) + best_ask(No)
  if cost < 1.0:
    profit = 1.0 - cost
    if profit > min_threshold:
      emit ArbOpportunity

多结果套利:
  cost = sum(best_ask(option_i)) for all options
  if cost < 1.0:
    profit = 1.0 - cost
    if profit > min_threshold:
      emit ArbOpportunity
```

**关键考量：**

- 用可执行深度（size）计算实际可获利金额，而非仅看最优价
- 考虑滑点：如果套利金额 > best_ask 的挂单量，需要吃到更深层级
- 套利窗口极短，检测必须在价格更新回调中同步完成

### 3.3 OrderExecutor — 订单执行

**Polymarket CLOB 下单流程：**

1. 构造订单参数（token_id, side, price, size）
2. 使用本地私钥对订单进行 EIP-712 签名
3. 通过 `POST /order` 提交到 CLOB
4. 轮询 `GET /order/{id}` 确认成交状态

**执行策略：**

```cpp
// 套利执行必须原子化 — 所有腿同时提交
class OrderExecutor {
public:
    // 并发提交套利的所有腿
    ExecutionResult execute_arb(const ArbOpportunity& opp);

private:
    // 单腿下单
    std::future<OrderResult> place_order(const ArbLeg& leg);

    // 异常处理：部分成交时的对冲
    void handle_partial_fill(const ExecutionResult& result);
};
```

**异常处理：**

- 某一腿下单失败 → 立即取消其他腿的挂单
- 部分成交 → 评估是否对冲或等待成交
- 网络超时 → 查询订单状态后决定重试或取消

### 3.4 RiskManager — 风控

```cpp
struct RiskConfig {
    double max_position_per_market;   // 单市场最大持仓 (USDC)
    double max_total_exposure;        // 总敞口上限
    double max_single_trade;          // 单笔交易上限
    double min_net_profit;            // 最小净利润阈值
    double max_slippage;              // 最大滑点容忍
    int max_open_orders;              // 最大挂单数
    double daily_loss_limit;          // 日亏损上限
    bool kill_switch;                 // 紧急停止开关
};
```

**风控检查（每笔交易前）：**

1. 单笔金额 ≤ max_single_trade
2. 当前市场持仓 + 新交易 ≤ max_position_per_market
3. 总敞口 ≤ max_total_exposure
4. 当日已实现亏损 < daily_loss_limit
5. kill_switch == false

### 3.5 WalletManager — 钱包管理

**本地私钥管理：**

- 私钥加密存储在本地文件中（AES-256 加密，启动时输入密码解锁）
- 运行时私钥仅在内存中，程序退出时清零
- 支持 EIP-712 签名（Polymarket CLOB 订单签名格式）

```cpp
class WalletManager {
public:
    // 启动时解锁
    bool unlock(const std::string& password);

    // 锁定（清除内存中的私钥）
    void lock();

    // EIP-712 签名
    std::string sign_order(const OrderParams& order);

    // 获取 USDC 余额
    double get_balance();

    // 获取 API Key (L2 认证)
    std::string get_api_key();

private:
    SecureBytes private_key_;  // 安全内存区域
    std::string address_;
    std::string api_key_;
    std::string api_secret_;
};
```

**Polymarket 认证体系：**

- L1 认证：钱包地址 + 签名（链上操作）
- L2 认证：API Key + Secret（CLOB 操作） — 通过 `POST /auth/api-key` 派生

### 3.6 PositionManager — 仓位管理

跟踪当前所有持仓和挂单。

```cpp
struct Position {
    std::string token_id;
    std::string market_id;
    double size;
    double avg_entry_price;
    double current_price;
    double unrealized_pnl;
};
```

**结算：**

- 市场到期后，持有的正确结果 token 价值 $1.00，错误结果 token 价值 $0
- 套利持仓（同时持有 Yes + No）在市场结算时保证盈利

### 3.7 Storage — 持久化

使用 SQLite 存储：

```sql
-- 交易记录
CREATE TABLE trades (
    id INTEGER PRIMARY KEY,
    market_id TEXT,
    token_id TEXT,
    side TEXT,
    price REAL,
    size REAL,
    order_id TEXT,
    arb_group_id TEXT,  -- 同一套利机会的交易分组
    status TEXT,
    created_at INTEGER,
    filled_at INTEGER
);

-- 套利机会记录
CREATE TABLE arb_opportunities (
    id INTEGER PRIMARY KEY,
    market_id TEXT,
    type TEXT,
    gross_profit REAL,
    net_profit REAL,
    executed INTEGER,     -- 0/1
    result TEXT,          -- SUCCESS / PARTIAL / FAILED
    detected_at INTEGER
);

-- 日度 PnL
CREATE TABLE daily_pnl (
    date TEXT PRIMARY KEY,
    realized_pnl REAL,
    fees_paid REAL,
    trades_count INTEGER
);
```

### 3.8 CLI Interface

```
$ ./polymarket-arb

╔══════════════════════════════════════════════════╗
║          Polymarket Arbitrage Bot v0.1           ║
╠══════════════════════════════════════════════════╣
║ Status: RUNNING     Balance: 1,250.00 USDC      ║
║ Markets: 142        Subscribed: 38               ║
║ Uptime: 02:15:33    Last check: 0.3s ago         ║
╠══════════════════════════════════════════════════╣
║ Today's PnL: +$12.35    Trades: 8                ║
║ Open positions: 3        Pending orders: 0       ║
╠══════════════════════════════════════════════════╣
║ Recent Opportunities:                            ║
║ 14:23:01  COMPLEMENT  "US Election" +0.8% ✓     ║
║ 14:20:45  MULTI       "FIFA Winner"  +1.2% ✓    ║
║ 14:18:12  COMPLEMENT  "BTC > 100k"  +0.3% skip  ║
╚══════════════════════════════════════════════════╝

Commands:
  status    — 显示详细状态
  positions — 显示当前持仓
  history   — 显示交易历史
  config    — 查看/修改配置
  pause     — 暂停交易
  resume    — 恢复交易
  quit      — 安全退出
>
```

---

## 4. 线程模型

```
Main Thread
  │
  ├── WebSocket Thread        — 接收实时数据推送
  │     └─→ 价格更新 → ArbDetector (同线程，低延迟)
  │                     └─→ 发现套利 → 投递到执行队列
  │
  ├── Execution Thread        — 从队列取套利机会，执行下单
  │     └─→ HTTP POST 下单 → 监控成交状态
  │
  ├── Housekeeping Thread     — 定时任务
  │     ├─→ 每 30s REST 全量校验价格
  │     ├─→ 每 60s 更新市场列表
  │     └─→ 每 5m 持久化统计数据
  │
  └── CLI Thread              — 用户输入处理
```

**线程间通信：**

- 使用无锁队列（lock-free queue）传递套利机会到执行线程
- 共享数据（价格缓存、仓位）使用读写锁（shared_mutex）

---

## 5. 费用计算

每笔套利的实际利润：

```
net_profit = gross_spread
           - maker_fee (0%)  或 taker_fee (按成交量阶梯)
           - gas_fee (Polygon 链上操作，极低 ~$0.01)
```

Polymarket 当前费用结构：
- Maker: 0%
- Taker: ~2%（按月成交量阶梯递减）
- Polygon gas: 通常 < $0.01

**套利可行性阈值：**

```
min_gross_spread > taker_fee_rate * 2 (两腿都 taker 的最坏情况)
```

如果能挂 Maker 单（限价单）等待成交，手续费降为 0%，但牺牲执行速度。

---

## 6. 依赖库

| 库 | 版本 | 用途 | 安装 |
|----|------|------|------|
| Boost.Beast | 1.84+ | WebSocket + HTTP | brew install boost |
| nlohmann/json | 3.11+ | JSON 解析 | 手动 header-only |
| OpenSSL | 3.x | ECDSA 签名 / AES 加密 | brew install openssl |
| SQLite3 | 3.x | 数据持久化 | macOS 自带 |
| spdlog | 1.13+ | 日志 | 手动编译 |
| fmt | 10+ | 格式化输出 | spdlog 依赖 |
| secp256k1 | latest | 以太坊签名 | brew install libsecp256k1 |
| ethash/keccak | — | Keccak256 哈希 | 手动集成 |

---

## 7. 目录结构

```
polymarket/
├── docs/
│   └── design.md              # 本文档
├── src/
│   ├── main.cpp               # 入口
│   ├── core/
│   │   ├── engine.h/cpp       # 核心引擎，线程管理
│   │   ├── market_feed.h/cpp  # 数据采集 (REST + WS)
│   │   ├── arb_detector.h/cpp # 套利检测
│   │   ├── order_executor.h/cpp # 下单执行
│   │   ├── risk_manager.h/cpp # 风控
│   │   ├── position_mgr.h/cpp # 仓位管理
│   │   └── wallet_mgr.h/cpp   # 钱包/签名
│   ├── net/
│   │   ├── http_client.h/cpp  # HTTP 客户端封装
│   │   ├── ws_client.h/cpp    # WebSocket 客户端封装
│   │   └── auth.h/cpp         # API 认证
│   ├── crypto/
│   │   ├── signer.h/cpp       # EIP-712 签名
│   │   ├── keccak.h/cpp       # Keccak256
│   │   └── keystore.h/cpp     # 加密私钥存储
│   ├── storage/
│   │   ├── database.h/cpp     # SQLite 封装
│   │   └── schema.sql         # 建表语句
│   ├── cli/
│   │   └── terminal.h/cpp     # CLI 界面
│   └── utils/
│       ├── config.h/cpp       # 配置加载
│       ├── logger.h/cpp       # 日志初始化
│       └── types.h            # 公共类型定义
├── config/
│   └── config.yaml            # 配置文件
├── tests/
│   └── ...
├── third_party/               # 手动管理的依赖
│   ├── json/
│   └── spdlog/
├── CMakeLists.txt
└── README.md
```

---

## 8. 配置文件

```yaml
# config/config.yaml

wallet:
  keystore_path: "./keystore.enc"
  address: "0x..."

polymarket:
  clob_rest_url: "https://clob.polymarket.com"
  clob_ws_url: "wss://ws-subscriptions-clob.polymarket.com/ws/market"
  chain_id: 137  # Polygon

strategy:
  min_net_profit_pct: 0.5       # 最小净利润 0.5%
  max_trade_size_usdc: 100.0    # 单腿最大下单金额
  arb_types:                    # 开启的套利类型
    - complement
    - multi_outcome
  min_liquidity_usdc: 500.0     # 最小市场流动性过滤
  min_volume_24h: 1000.0        # 最小24h成交量过滤

risk:
  max_position_per_market: 500.0
  max_total_exposure: 2000.0
  daily_loss_limit: 50.0
  max_open_orders: 10
  kill_switch: false

logging:
  level: "info"                 # debug / info / warn / error
  file: "./logs/bot.log"
  console: true
```

---

## 9. 开发计划

> **工作流约定：** 每步完成后，在 `docs/steps/step-{N}-report.md` 写执行简报，停下来等用户检查。用户确认后提交 git 并更新 memory，再进入下一步。  
> checkbox 打勾 `[x]` 表示已完成并经用户确认。

### Phase 1 — 基础设施

- [ ] **1.1** CMake 项目搭建 + 依赖集成
- [ ] **1.2** HTTP 客户端封装 (Boost.Beast)
- [ ] **1.3** WebSocket 客户端封装
- [ ] **1.4** JSON 解析工具
- [ ] **1.5** 配置加载 (YAML)
- [ ] **1.6** 日志系统

### Phase 2 — 数据采集

- [ ] **2.1** REST API 对接：获取市场列表、价格
- [ ] **2.2** WebSocket 订阅：实时价格更新
- [ ] **2.3** 本地订单簿缓存
- [ ] **2.4** 数据校验机制

### Phase 3 — 钱包与认证

- [ ] **3.1** 本地密钥加密存储
- [ ] **3.2** Keccak256 / secp256k1 签名
- [ ] **3.3** EIP-712 结构化签名
- [ ] **3.4** CLOB API 认证 (API Key 派生)

### Phase 4 — 套利核心

- [ ] **4.1** 互补事件套利检测
- [ ] **4.2** 多结果套利检测
- [ ] **4.3** 深度感知的利润计算
- [ ] **4.4** 下单执行 + 并发提交
- [ ] **4.5** 异常处理（部分成交、超时）

### Phase 5 — 风控与仓位

- [ ] **5.1** 风控规则引擎
- [ ] **5.2** 仓位跟踪
- [ ] **5.3** SQLite 持久化
- [ ] **5.4** 日度 PnL 统计

### Phase 6 — CLI 与优化

- [ ] **6.1** 终端界面
- [ ] **6.2** 命令系统
- [ ] **6.3** 性能优化（延迟测量、瓶颈分析）
- [ ] **6.4** 模拟交易模式 (dry-run)

---

## 10. 风险与注意事项

1. **套利窗口极短** — Polymarket 流动性集中，套利机会可能在毫秒内消失，需要接受大量"发现但未执行成功"的情况
2. **Taker 费率侵蚀利润** — 2% 的 taker fee 意味着 gross spread < 4% 的互补套利不可行（两腿都吃 taker）
3. **API 限流** — CLOB API 有速率限制，需要合理控制请求频率
4. **市场结算风险** — 持仓直到市场结算的时间成本（资金锁定）
5. **私钥安全** — 本地管理私钥务必做好加密和权限控制
6. **合规性** — 确认当地法律法规允许使用此类工具
