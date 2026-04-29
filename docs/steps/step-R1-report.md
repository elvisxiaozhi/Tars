# Step R1 — 实盘下单分支 + 骨架

## 概要

新建 `live-trading` 分支（基于 `feature@b787586`），落地实盘下单的最小骨架：`LiveTrader` 门面类（所有方法 stub throw）+ `PolygonConfig` 配置块 + main.cpp 启动期 live 模式守卫（暂时拒启动）。dry_run 路径完全不受影响。

## 关键命令

```bash
git checkout -b live-trading            # 新分支
cmake --build build -j                  # 编译验证
./build/polymarket-arb                  # dry_run 正常启动
# 把 config 里 mode 改成 "live" → 启动后立即 fail-fast 退出
```

## 文件清单

| 操作 | 文件 |
|------|------|
| 新增 | `src/core/live_trader.h` |
| 新增 | `src/core/live_trader.cpp` |
| 修改 | `src/utils/config.h`（新增 `PolygonConfig`，挂到 `AppConfig`）|
| 修改 | `src/utils/config.cpp`（加载 `polygon` 块）|
| 修改 | `src/main.cpp`（init_logging 后插入 live 模式拒启动守卫）|
| 修改 | `CMakeLists.txt`（加 `live_trader.cpp` 到 SOURCES）|
| 修改 | `config/config.example.json`（新增 `polygon` 示例块）|
| 新增 | `docs/steps/step-R1-report.md` |

## 设计决策

### 1. 为什么单独建分支 `live-trading` 而不在 feature 上做？

实盘代码风险等级 = 「真金白银」。在独立分支上演化，feature 继续跑 dry_run 观察策略效果，互不污染。R10 验收通过后再决定合并节奏。

### 2. 为什么 R1 不预埋 main.cpp 内部的 if-live 分支？

考虑过两种方案：

- **方案 A**：在 main.cpp 各下单点（入场、TP 卖、止损、到期平仓）预埋 `if (mode=="live") trader.xxx() else { /*existing*/ }`
- **方案 B**：仅在 main() 顶部加 startup gate，dry_run 代码原封不动；R7/R8 实现时一次性完成「埋分支 + 填实现」

选 B。理由：方案 A 会在 R1 阶段就插入若干 stub 点位，但 stub 的入参 / 返回类型现在还不能完全敲定（订单 ID 字段、partial fill 处理方式等需要 R5 鉴权完成后才能确定）。**晚做不漏做**。

### 3. 为什么 LiveTrader 用 throwing stub 而不是 `// TODO` 空实现？

dry_run 阶段 LiveTrader 的方法不会被调用；即便将来有人误把守卫去掉，throw 能在第一个非法调用处立即崩，不会让 bot 跑出去半成交。

### 4. PolygonConfig 字段为什么放空默认值？

合约地址（USDC / CTF / Exchange）从 docs.polymarket.com 实时抄写，不预填避免拷贝错误地址造成迷之 transaction。R3 实现时会校验非空。

## 后续 step 路线（信息闭环）

| Step | 内容 | LiveTrader 方法 | 风险 |
|------|------|-----------------|------|
| R2 | 钱包加载（keystore.enc → 地址）| `init_wallet`, `wallet_address` | 0 |
| R3 | 链上读（USDC/CTF/allowance）| `read_chain_state` | 0 |
| R4 | EIP-712 签名核心模块（独立 unit） | — | 0 |
| R5 | CLOB API key 鉴权 | `ensure_clob_authenticated` | 低 |
| R6 | Approval 校验 | `check_approvals_sufficient` | 低（独立 CLI 工具发起 approve）|
| R7 | 入场限价单 | `place_entry_order` | **高（首次实盘）** |
| R8 | 出场（partial TP + 止损 market）| `place_exit_order` | 高 |
| R9 | 启动对账 + SIGINT 应急平仓 | `reconcile_on_startup`, `emergency_close_all` | 中 |
| R10 | 端到端验收（$5）| — | — |

## 红线遵守

- [x] 未引入硬编码密钥 / 合约地址（地址留空，由 config 填）
- [x] 未跳过错误处理（stub 显式 throw 而非空跑）
- [x] 编译通过，无 warning
- [x] dry_run 行为零变化（smoke test 验证）

## 验收结果

**编译：**
```
[ 75%] Building CXX object CMakeFiles/polymarket-arb.dir/src/core/live_trader.cpp.o
[ 80%] Linking CXX executable polymarket-arb
[100%] Built target polymarket-arb
```

**dry_run 启动正常：**
```
[2026-04-30 00:05:24.582] [info] polymarket-arb v0.3.0 [DRY RUN]
[2026-04-30 00:05:24.596] [info] Strategy loop: poll every 5s, account=$20, dashboard at http://localhost:9090
[2026-04-30 00:05:32.252] [info] BTC: $76013.92 | strike: $75924.24 | dev: +0.12% | ...
[2026-04-30 00:05:37.032] [info] #1 | 54min | BTC $76014 +0.12% | ...
```

**live 模式被拒：**
```
[2026-04-30 00:05:07.424] [info] polymarket-arb v0.3.0 [LIVE]
[2026-04-30 00:05:07.425] [error] LIVE MODE 暂未就绪（当前进度：R1 骨架）
[2026-04-30 00:05:07.425] [error] 待完成：R2 钱包 / R3 链上读 / R4 EIP-712 / R5 CLOB 鉴权 /
[2026-04-30 00:05:07.425] [error]         R6 approvals / R7 入场 / R8 出场 / R9 对账+应急平仓
[2026-04-30 00:05:07.425] [error] 请将 config 中 strategy.mode 改回 "dry_run" 后再启动。
exit code: 1
```

## 遗留问题

无。R1 是地基步骤，没有未完成的本步任务。
下一步：R2 钱包模块（keystore 解密 + secp256k1 派生地址 + known-answer test）。
