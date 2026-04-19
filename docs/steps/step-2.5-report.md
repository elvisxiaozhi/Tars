# Step 2.5 — Web Dashboard + API Server

## 概要

在 C++ 程序中内嵌 HTTP 服务器，提供 REST API 和前端 dashboard 页面。单二进制部署，浏览器访问即可监控策略运行状态。

## 关键命令

```bash
cmake --build build
./build/polymarket-arb              # 启动后访问 http://localhost:9090
```

## 文件清单

| 操作 | 文件 |
|------|------|
| 新增 | `src/net/api_server.h/cpp` — Boost.Beast 内嵌 HTTP 服务器 |
| 新增 | `src/dashboard.h` — 前端 HTML/CSS/JS（嵌入 C++ 字符串） |
| 修改 | `src/main.cpp` — 集成 API server，注册数据回调，共享状态 |
| 修改 | `src/utils/config.h/cpp` — 新增 `network.api_port` |
| 修改 | `config/config.json` — 新增 `api_port: 9090` |
| 修改 | `CMakeLists.txt` — SOURCES 增加 api_server.cpp |

## API 端点

| 端点 | 说明 |
|------|------|
| `GET /` | Dashboard 页面 |
| `GET /api/status` | BTC 价格、波动率、持仓、运行时间、tick 计数 |
| `GET /api/trades` | 交易历史（全部字段） |
| `GET /api/stats` | 胜率、总 P&L、日 P&L、胜败数 |

## 设计决策

- **内嵌 HTTP server**：用 Boost.Beast（已有依赖），不引入新框架，单二进制部署
- **HTML 嵌入 C++ 字符串**：`dashboard.h` 用 raw string literal，无需静态文件目录
- **共享状态 + mutex**：策略线程写，API 线程读，避免竞态
- **非阻塞 accept**：server 线程每 100ms 检查 running_ 标志，支持优雅退出
- **CORS 开放**：`Access-Control-Allow-Origin: *`，方便本地/远程访问
- **3 秒自动刷新**：前端 JS 每 3 秒轮询 API，实时更新

## Dashboard 展示内容

- BTC 实时价格、strike、偏离率
- 总 P&L 和日 P&L
- 胜率和交易数
- 当前波动率 vs 24h 均值
- K线剩余时间
- 持仓详情（入场价、当前价、未实现盈亏）
- 交易历史（时间、方向、入场/出场价、原因、P&L）

## 红线遵守

- [x] 没有引入硬编码密钥
- [x] 编译通过，零 warning
- [x] API server 独立线程，不阻塞策略循环

## 验收结果

```
API server listening on http://localhost:9090

GET /api/status → 200
{
    "btc_price": 75169.72,
    "btc_strike": 75257.59,
    "btc_deviation_pct": -0.12,
    "current_vol": 0.0013,
    "avg_vol": 0.0042,
    "minutes_remaining": 44,
    "mode": "dry_run",
    "open_positions": 0,
    "tick_count": 1,
    "uptime": "00:00:17"
}

GET /api/stats → 200
GET /api/trades → 200
GET / → 200 (dashboard HTML)
```

## 遗留问题

- 服务器部署需要考虑：反向代理（nginx）、HTTPS、认证
- 后续可增加：P&L 曲线图、信号拒绝日志、config 热更新接口
