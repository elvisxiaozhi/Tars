# Step 2.1 — MarketFeed REST 全量拉取

## 概要

实现 MarketFeed 模块：全量拉取市场列表，用 token price 做套利初筛，对候选市场拉取订单簿，终端打印报价和套利扫描结果。

## 关键命令

```bash
cmake --build build
./build/polymarket-arb              # 使用默认 config/config.json
./build/polymarket-arb config.json  # 指定配置
```

## 文件清单

| 操作 | 文件 |
|------|------|
| 新增 | `src/core/market_feed.h` |
| 新增 | `src/core/market_feed.cpp` |
| 修改 | `src/main.cpp` — 调用 MarketFeed 流程 + 自动查找 config 路径 |
| 修改 | `CMakeLists.txt` — SOURCES 增加 market_feed.cpp |
| 修改 | `config/config.json` — 新增 `max_markets_to_scan`、`network.proxy_url` 配置 |
| 修改 | `src/utils/config.h` / `config.cpp` — 新增 `NetworkConfig`、解析 network 段 |
| 修改 | `src/net/http_client.h` / `http_client.cpp` — 构造函数接受 proxy_url，优先配置 fallback 环境变量 |
| 修改 | `src/net/ws_client.h` / `ws_client.cpp` — 同上 |

## 设计决策

- **两阶段扫描**：先用 token 自带 price 做快速初筛（0 网络开销），再只对有潜力的市场拉订单簿
- **max_markets_to_scan 配置**：限制拉取订单簿的数量（默认 100），避免对 6000+ 市场全部拉取（通过代理要数小时）
- **按 price_sum 升序排序**：套利潜力最大的市场优先拉取订单簿
- **30ms 限流间隔**：避免触发 API 429 rate limit
- **分页拉取市场**：通过 next_cursor 自动翻页获取全部市场
- **config 路径自动查找**：从可执行文件路径向上最多 5 级查找 `config/config.json`，兼容 Xcode 启动
- **proxy 配置化**：`network.proxy_url` 写入配置文件，不再依赖 shell 环境变量，Xcode/CLI 均可用

## 红线遵守

- [x] 没有引入硬编码密钥
- [x] API 限流保护（30ms 间隔 + 429 重试）
- [x] 编译通过，零 warning

## 验收结果

```
Markets loaded: 6164 active out of 6164 total
Price scan: 6164 markets | sum<1.0: 659 | sum~1.0: 5505 | sum>1.0: 0

Top candidates by token price sum:
  Will SpaceX or OpenAI IPO first?                   0.000   0.061   0.061 ***
  Call of Duty: OpTic Texas vs Cloud9 New York...     0.000   0.090   0.090 ***
  ...

100 markets selected for order book fetch
Order books: 200 success, 0 failed, 651s elapsed

No arbitrage opportunities found in order books
```

- 6164 个市场拉取成功
- 659 个市场 token price sum < 1.0（初筛候选）
- 100 个市场的订单簿全部拉取成功
- 初筛候选实际是已结算/无流动性市场，确认无真实套利机会

## 遗留问题

- 通过代理拉 100 个市场的订单簿需 ~11 分钟（每次 HTTP 请求含代理延迟 ~3s），后续可考虑并发请求或直连
- price_sum < 1.0 的市场大多是已结算的体育/电竞赛事（一方 token 无流动性），需要更好的过滤策略（如排除 enable_order_book=false 的市场）
- 真正的套利机会可能出现在 price_sum ≈ 1.0 的高流动性市场中（ask 价微小偏差），需要 WS 实时监控才能捕获
