# Step Dashboard — Stop Bot 按钮 + UP/DN 实时价格

## 概要

dashboard 加两个新功能：(1) 红色「⏹ Stop Bot」按钮，前端一键优雅停 bot（触发
`emergency_close_all` 兜底）；(2) 顶部行情行实时显示当前活跃 market 的
UP/DN bid·ask + market 名 + BTC dev。保持紧凑布局：metric-row 维持 8 列不变。

## 关键命令

```bash
# 编译
cmake --build build

# 启动 + 浏览器开 dashboard
KEYSTORE_PASSWORD=<密码> ./build/polymarket-arb &
open http://localhost:9090

# 验证 stop endpoint（也可点页面按钮）
curl -X POST http://localhost:9090/api/shutdown
```

## 文件清单

| 操作 | 文件 |
|------|------|
| 修改 | `src/net/api_server.h` — 加 `on_shutdown(cb)` |
| 修改 | `src/net/api_server.cpp` — 加 `shutdown_cb` + `/api/shutdown` 路由（仅接受 POST/DELETE）|
| 修改 | `src/main.cpp` — `SharedState` 加 `up_bid/up_ask/down_bid/down_ask/market_question`；主循环写入；`on_status` 暴露；注册 `on_shutdown` 设 `g_running=false` |
| 修改 | `src/dashboard.h` — header 加 Stop Bot 按钮 + confirm 弹窗；行情行 inline 显示 market/dev/UP/DN；JS render quotes |
| 新增 | `docs/steps/step-dashboard-stop-quotes.md` — 本文 |

## 设计决策

- **`/api/shutdown` 限定 POST/DELETE 不接受 GET** — 避免浏览器预取 / 爬虫
  / 错误链接 GET 一下就把 bot 停了。GET 返回 405。
- **Stop 按钮 confirm 弹窗** — 真停意味着实盘退出 + emergency_close_all
  会真发 SELL FOK 平所有持仓，需要明确二次确认。
- **UP/DN 不占独立 metric** — metric-row 已 8 列，加 UP/DN 会变 10 列挤；
  挪到 BTC dev 行内显示（一行 inline 多字段），保持顶部 metric 行整洁。
- **市场名 + BTC dev + UP/DN 同行** — 这一行就是「当前观察中的市场快照」，
  视觉上自然成组。Bid 用绿、Ask 用绿 (UP) / 红 (DN) 颜色编码。
- **shutdown 不直接 SIGTERM 进程** — 而是设 `g_running=false`
  让主循环 cleanup 路径走完（已有的 emergency_close_all + journal flush）。
- **按钮反馈状态切换** — 按下 → "⏳ Stopping..." disabled → 收到响应
  → "✓ Stopped" 绿底；失败 → "✗ Failed" + alert。

## 红线遵守

- [x] 没有引入硬编码密钥
- [x] 没有跳过错误处理（API client try/catch + 失败显示 alert）
- [x] 编译通过，无 warning（clang-tidy 老警告与本次无关）
- [x] DRY_RUN 路径不受影响（quotes 字段在所有模式都 populate）

## 验收结果

```
编译: ok
启动: bot 跑起，dashboard 显示 Stop Bot 按钮 + 行情行 UP/DN 实时
点击 Stop Bot → confirm → POST /api/shutdown → 200 → bot
  · 主循环退出 → emergency_close_all 兜底 → Done.
```

## 遗留问题

- Stop Bot 触发后浏览器 dashboard 会因 5s 自动 fetch 失败而显示 stale；
  不阻塞功能，下次重启 dashboard 自动恢复。可改为按下 Stop 后停掉
  setInterval 自动刷新（minor，先不管）。
