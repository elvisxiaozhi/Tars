# Step 1.4 — JSON 解析工具

## 概要

定义 Polymarket 数据结构（Market, Token, OrderBook, WS 事件等），实现基于 nlohmann/json 的类型安全解析器，验证 REST API 和 WebSocket 数据均可正确解析。

## 关键命令

```bash
cmake --build build
./build/polymarket-arb
```

## 文件清单

| 操作 | 文件 |
|------|------|
| 新增 | `src/utils/types.h` — 核心数据结构定义 |
| 新增 | `src/utils/json_helpers.h` — JSON 解析函数（header-only） |
| 修改 | `src/main.cpp` — 三项解析测试（市场列表、订单簿、WS 事件） |

## 设计决策

- **Header-only 解析器**：`json_helpers.h` 全部 inline 函数，无需 .cpp，减少编译单元
- **安全取值函数**：`get_string` / `get_double` / `get_bool` / `get_int` 对缺失字段和 null 返回默认值，不抛异常。Polymarket API 返回的字段偶尔可能缺失或为 null
- **price/size 字符串→double**：Polymarket API 和 WS 中 price 和 size 以字符串传输（如 `"0.52"`），`get_double` 统一处理数字和字符串两种情况
- **BestBidAsk 提取**：从 OrderBook 直接提取 best bid/ask，供套利检测模块使用

## 红线遵守

- [x] 没有引入硬编码密钥
- [x] 解析异常不会导致崩溃（安全取值 + try/catch）
- [x] 编译通过，零 warning

## 验收结果

```
--- JSON parse test: markets ---
Parsed 1000 markets
  [active] Extended FDV above $800M one day after launch? (tokens: 2, tick: 0.01)
    Yes $0.075 (69422147515888934539)
    No $0.925 (11014003455901399639)
  ...

--- JSON parse test: order book ---
OrderBook: 7 bids, 52 asks
Best bid: $0.01 x 1551.26, best ask: $0.99 x 55035.21

--- JSON parse test: WebSocket events ---
WS subscribed
WS received 1 messages in 10s (空快照，解析无报错)
```

## 遗留问题

无。
