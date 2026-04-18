# Step 1.5 — 配置加载 (JSON) + 日志初始化

## 概要

实现 JSON 配置文件加载和 spdlog 日志系统初始化（控制台 + 文件轮转），同时更新了开发计划（Phase 2 调整为数据采集+套利检测三步走）。

## 关键命令

```bash
cmake --build build
./build/polymarket-arb                     # 默认读 config/config.json
./build/polymarket-arb path/to/config.json # 指定配置路径
```

## 文件清单

| 操作 | 文件 |
|------|------|
| 新增 | `src/utils/config.h` — AppConfig 结构体 + load_config / init_logging 声明 |
| 新增 | `src/utils/config.cpp` — JSON 配置解析 + spdlog 多 sink 初始化 |
| 新增 | `config/config.json` — 默认配置文件 |
| 修改 | `src/main.cpp` — 用配置驱动，精简为启动流程 |
| 修改 | `CMakeLists.txt` — SOURCES 增加 config.cpp |
| 修改 | `docs/design.md` — Phase 2 调整为 2.1 REST报价 → 2.2 WS实时 → 2.3 套利扫描 |

## 设计决策

- **JSON 而非 YAML**：复用已有 nlohmann/json，零额外依赖
- **所有字段有默认值**：配置文件中缺少任何字段都不会崩溃
- **日志双输出**：控制台 (color) + 文件 (10MB 轮转, 保留 3 份)
- **命令行参数指定配置路径**：`argv[1]` 可选覆盖默认路径
- **合并原 1.5 + 1.6**：配置和日志初始化紧耦合，合并为一步

## 红线遵守

- [x] 没有引入硬编码密钥（wallet.address 在配置文件中为空）
- [x] 配置文件缺失时报错退出，不静默跳过
- [x] 编译通过，零 warning

## 验收结果

```
[2026-04-18 22:43:15.141] [info] polymarket-arb v0.1.0
[2026-04-18 22:43:15.141] [info] config loaded from: config/config.json
[2026-04-18 22:43:15.141] [info] CLOB REST: https://clob.polymarket.com
[2026-04-18 22:43:15.141] [info] CLOB WS:   wss://ws-subscriptions-clob.polymarket.com/ws/market
[2026-04-18 22:43:15.141] [info] min profit: 0.5%, max trade: $100
[2026-04-18 22:43:15.141] [info] risk: daily_loss_limit=$50, kill_switch=OFF
[2026-04-18 22:43:18.356] [info] CLOB /time: 1776523396 (status=200)
[2026-04-18 22:43:18.356] [info] ready
```

- 日志文件 `logs/bot.log` 同步写入
- 错误配置路径正确报错退出

## 遗留问题

无。
