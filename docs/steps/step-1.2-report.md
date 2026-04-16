# Step 1.2 — HTTP 客户端封装 (Boost.Beast)

## 概要

基于 Boost.Beast 实现 HTTPS 客户端，支持 GET/POST/DELETE，自动检测系统 HTTP 代理并通过 CONNECT 隧道访问。

## 关键命令

```bash
cmake --build build
./build/polymarket-arb
```

## 文件清单

| 操作 | 文件 |
|------|------|
| 新增 | `src/net/http_client.h` |
| 新增 | `src/net/http_client.cpp` |
| 修改 | `src/main.cpp` — 增加 HTTP 测试（请求 Polymarket /time） |
| 修改 | `CMakeLists.txt` — SOURCES 增加 http_client.cpp |

## 设计决策

- **Pimpl 模式**：header 不暴露 Boost.Beast 类型，减少编译依赖传播
- **CONNECT 代理支持**：开发过程中发现本机有 HTTP 代理 (127.0.0.1:7897)，Polymarket 直连被 reset。通过读取 `HTTPS_PROXY` / `HTTP_PROXY` 环境变量，自动建立 CONNECT 隧道
- **同步 HTTP**：当前用同步调用，后续 WebSocket 模块会引入 asio event loop，届时可考虑异步化
- **每次请求新建连接**：简单可靠，后续如需连接池可在 Impl 层加

## 红线遵守

- [x] 没有引入硬编码密钥
- [x] TLS 开启了 verify_peer + SNI
- [x] 编译通过，零 warning

## 验收结果

```
[2026-04-17 01:07:17.074] [info] --- HTTP client test ---
[2026-04-17 01:07:17.638] [info] GET /time status=200
[2026-04-17 01:07:17.639] [info] body: 1776359237
```

通过 CONNECT 代理成功请求 `https://clob.polymarket.com/time`，往返延迟 ~560ms（含代理开销）。

## 遗留问题

- 无连接复用，每次请求都新建 TCP+TLS，后续高频场景可能需要连接池
- plain HTTP 路径的代理处理暂时也走 CONNECT，够用但不标准（标准做法是改写请求 target 为完整 URL）
