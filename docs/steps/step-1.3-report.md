# Step 1.3 — WebSocket 客户端封装

## 概要

基于 Boost.Beast 实现 WSS 客户端，支持 CONNECT 代理隧道，回调模式收消息，成功接收 Polymarket 实时订单簿和价格变动数据。

## 关键命令

```bash
cmake --build build
./build/polymarket-arb
```

## 文件清单

| 操作 | 文件 |
|------|------|
| 新增 | `src/net/ws_client.h` |
| 新增 | `src/net/ws_client.cpp` |
| 修改 | `src/main.cpp` — 增加 WebSocket 测试（订阅 Polymarket 活跃市场） |
| 修改 | `CMakeLists.txt` — SOURCES 增加 ws_client.cpp |

## 设计决策

- **回调模式**：on_message / on_error / on_connect / on_close，调用方注册回调后 `run()` 阻塞读消息循环
- **Pimpl 隔离**：header 不暴露 Beast/Boost 类型
- **CONNECT 代理复用**：与 HTTP 客户端相同的代理检测和 CONNECT 隧道逻辑
- **`run()` 在独立线程中调用**：主线程 connect + send 订阅，ws_thread 跑消息循环，close() 线程安全
- **Polymarket WS 订阅格式**：查阅官方文档确认为 `{"type":"market","assets_ids":["token_id_1","token_id_2"]}`

## 红线遵守

- [x] 没有引入硬编码密钥
- [x] TLS verify_peer + SNI
- [x] 编译通过，零 warning

## 验收结果

```
[01:19:16.898] WS subscribed to active market
[01:19:17.151] WS msg #1: [{"market":"0x813451b...","asset_id":"11057785...","timestamp":"1776359934366",...  (book snapshot)
[01:19:21.320] WS msg #2: {"market":"0x813451b...","price_changes":[{"asset_id":"72231738...","price":"0...  (price_change)
[01:19:23.387] WS msg #3: {"market":"0x813451b...","price_changes":[{"asset_id":"72231738...","price":"0...  (price_change)
[01:19:26.900] WS received 3 messages in 10s, closing...
[01:19:27.151] WS connection closed normally
```

10 秒内成功收到 1 条订单簿快照 + 2 条价格变动事件。

## 遗留问题

- 没有自动重连机制，断连后需要上层处理重建连接
- `send()` 和 `close()` 虽然标记线程安全，但 Beast stream 本身不是线程安全的，后续如需并发写入需加锁
