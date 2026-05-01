# 运维手册

按"我要做 X"组织。日志关键词速查见 `docs/log_reference.md`，本文不重复。
出问题时也看一眼 `docs/DECISIONS.md`——很多反直觉行为都有事故背景，别按"看着不对"修。

---

## 1. 第一次 setup

### 1.1 编译依赖
```bash
brew install boost openssl@3 secp256k1 cmake
```
spdlog 走 git submodule（已在 `third_party/`）；nlohmann/json 是 header-only。

### 1.2 编译
```bash
cmake -S . -B build && cmake --build build -j
```
产物：
- `build/polymarket-arb` — 主程序
- `build/import_key` — 一次性私钥导入工具
- `build/test_crypto / test_chain / test_eip712` — KAT 测试

### 1.3 配置
```bash
cp config/config.example.json config/config.json
# 编辑 config/config.json，至少填:
#   strategy.mode = "dry_run"  （第一次默认 dry）
#   network.proxy_url = "http://127.0.0.1:7897"  （或留空走 env）
```
LIVE 模式额外字段在 §3.1 说。

### 1.4 跑起来
```bash
./build/polymarket-arb               # 默认找 ./config/config.json
./build/polymarket-arb /abs/path.json # 显式指定
open http://127.0.0.1:9090           # dashboard
```

---

## 2. dry_run ↔ live 切换检查清单

切 live 是单向门——切换前**逐项打钩**，切回 dry 只需改 mode 字段。

### 2.1 切去 live 前

```
[ ] keystore.enc 已生成且能解锁         （§3.2）
[ ] config/config.json 三层账户填全：
    [ ] wallet.address           = EOA
    [ ] polymarket.proxy_address = Simple7702Account
    [ ] polymarket.api_address   = CLOB profile API address
[ ] config.strategy.mode = "live"
[ ] config.strategy.account_balance 与真实 vault cash 一致或更小
[ ] polygon.rpc_urls 留空或填可用 RPC
[ ] kill_switch / consecutive_loss / daily_drawdown 检查已重新启用
    （DECISIONS.md S4：dry_run 测试期默认关掉，切 live 必须打开）
[ ] 当前 vault 没有未平仓订单（启动时 reconcile 会列出）
[ ] dashboard 端口 9090 没被占用
```

### 2.2 切回 dry

```
[ ] config.strategy.mode = "dry_run"
[ ] （可选）若 paper P&L 与历史 live 数据混杂，备份 logs/trades.jsonl
```
切回 dry 不影响 keystore.enc / api 凭证，下次再切 live 不用重导。

---

## 3. LIVE 模式日常操作

### 3.1 必填配置字段
| 字段 | 来源 |
|---|---|
| `wallet.address` | 你的私钥 EOA 地址（`import_key` 派生时会校验） |
| `wallet.keystore_path` | 默认 `./keystore.enc` |
| `polymarket.proxy_address` | polymarket.com → Deposit 页可见 |
| `polymarket.api_address` | polymarket.com → Profile Settings → "API use only" |
| `polygon.rpc_urls` | 留空走代码内置 publicnode/drpc/1rpc failover |

### 3.2 keystore 导入 / 重新生成
```bash
# 1. 把明文私钥写到临时文件（只能从文件读，不接受命令行参数）
echo "0x..." > /tmp/pk.txt
chmod 600 /tmp/pk.txt

# 2. 派生 + 加密（密码 ≥12 位，二次确认）
./build/import_key /tmp/pk.txt ./keystore.enc 0x356E4c0a80B5...   # 第三参数是 expected EOA

# 3. 立刻清理明文
shred -uvz /tmp/pk.txt   # macOS 没 shred 就 rm + 清剪贴板
history -c               # 清 shell history
```
keystore.enc 已加入 `.gitignore`。`import_key` 会 `chmod 0600`，文件只 owner 可读。

**密码忘了**：没办法恢复，必须重新走 §3.2 流程（密码作为 scrypt 派生 key，非可逆）。

### 3.3 启动守卫失败诊断
启动横幅报哪步失败就看对应行：

| 错误前缀 | 含义 | 第一步检查 |
|---|---|---|
| `wallet init failed` | 解 keystore 失败 / 派生地址 ≠ config.wallet.address | 密码错？config 里 address 写错？ |
| `chain read failed` | Polygon RPC 全失败 | `polygon.rpc_urls` 网络通？代理跑着？ |
| `CLOB auth failed` | EIP-712 签名 → /auth/api-key 拒 | 看下一行提示，常见：HTTP 401 → `address` 字段类型对错（main.cpp L214 已写明）；`api_address` 与 EOA 不匹配 |
| `polymarket balance read failed` | `/balance-allowance` 拒 | L2 HMAC 凭证（R5 拿到的三件套）问题；CLOB session 没建好 |
| `reconcile failed` | `/data/orders` 拒 | 同上，L2 凭证；或 server 5xx，过会儿重试 |

任一步失败 → `return 1`，**不要**改代码绕过守卫（DECISIONS.md A4）。

### 3.4 看真实 cash 余额
- dashboard 顶栏 Balance（dry_run 是虚拟 risk balance，live 是 `cached_cash_pusd`，每 5 分钟刷一次）
- 日志：`POL: cash=$X.XX`（`refresh_balance_if_stale` 触发后打）
- 强制刷新：暂时没暴露 CLI——重启 bot 即可（启动守卫会读一次）

---

## 4. 排障常见问题

### 4.1 dashboard 打不开 / 502
1. 确认 bot 还在跑（`ps | grep polymarket-arb`）
2. 端口冲突：改 `network.api_port`（默认 9090）
3. 防火墙挡了 127.0.0.1（macOS 几乎不会）

### 4.2 HTTP 调用挂死 / strategy loop 卡住
- DECISIONS.md D2 已加 35s wall-clock guard，理论上不会再 28 分钟挂死
- 真卡了：看是不是新加的 HTTP 路径没走 `HttpClient`（绕过了兜底）
- 临时手段：SIGINT 退出会触发 `emergency_close_all`；如果 SIGINT 也卡，`kill -9` 后**手动去 polymarket.com 平仓**

### 4.3 LIVE BUY 反复 timeout（60s 不 fill）
- ask 边没流动性：本场放弃属正常（DECISIONS.md B2）
- 看日志 `LIVE BUY didn't fill in 60s (status=...) cancelling`——`status` 字段告诉你订单当时啥状态
- 如果是 strategy 给的入场价偏低（low ask 时市场已经在跌），等下一根 K 线

### 4.4 paper P&L 和钱包余额对不上
- DECISIONS.md B4 已修：fill 后用真实成交价覆盖逻辑价
- 仍对不上的可能：（1）warning 日志里有 "no fill amount in response"——response 字段缺；（2）老记录是 B4 修复前的，accept 历史误差，新数据对账即可
- 强制对账：重启 bot 触发 `read_polymarket_balance` 拿 server 真值

### 4.5 启动时 reconcile 报有未平仓订单
- 日志会列出 `OpenOrder { order_id, token_id, side, price, size, status }`
- 这些是上次 bot 退出时没 cancel 干净的（理论上不该有，DECISIONS.md A4 应该清掉）
- 处理：（a）等它们自然 fill 或过期；（b）去 polymarket.com UI 手动 cancel；（c）写一次性脚本调 `cancel_order`（参考 `tools/import_key.cpp` 风格）

### 4.6 编译失败
| 报错 | 原因 |
|---|---|
| `Could NOT find Boost` | `brew install boost`，Apple Silicon 默认 `/opt/homebrew/opt/boost` |
| `Could NOT find OpenSSL 3.0` | `brew install openssl@3`（不是 openssl@1.1） |
| `secp256k1.h: No such file` | `brew install secp256k1` |
| spdlog 链接错误 | `git submodule update --init third_party/spdlog` |

`build/compile_commands.json` 已生成（CMake `EXPORT_COMPILE_COMMANDS ON`），可给 clangd / IDE 用。

---

## 5. 数据维护

### 5.1 重置交易历史
```bash
# 备份后清空（保留文件，让 TradeJournal 启动时空读）
mv logs/trades.jsonl logs/trades.jsonl.bak.$(date +%Y%m%d)
touch logs/trades.jsonl
```
**注意**：dashboard 下次启动会显示 0 笔历史；本次 session 仍正常记录。

### 5.2 仅看本会话总结
退出时 `print_summary` 默认就是 session-only（DECISIONS.md O1，commit `43846cf`），不用配置。

### 5.3 修历史 mode 标签
老记录加载时 mode 缺省 = `"dry_run"`，但实际可能是 live：
```bash
.venv/bin/python tools/patch_live_mode.py
# 扫 logs/bot.log 里的 LIVE close 事件 → 改 trades.jsonl 对应记录的 mode
# 备份到 logs/trades.jsonl.bak_modefix
```
当前 mode 分布检查：
```bash
.venv/bin/python tools/check_modes.py
```

### 5.4 日志滚动
spdlog 当前未配 rotation。简单粗暴：
```bash
# bot 退出时手动 archive
mv logs/bot.log logs/bot.log.$(date +%Y%m%d)
```
长期：在 `init_logging` 里换成 `rotating_file_sink_mt`。

---

## 6. 紧急应急

### 6.1 立刻停 bot 但**保留**仓位
```bash
# SIGINT 会触发 emergency_close_all（自动平仓）— 不是你想要的
# 替代：直接 kill -9
kill -9 <pid>
```
**注意**：bot tracked positions 会丢，下次启动 reconcile 时仅能看到 server 侧未成交订单——已成交的仓位需要去 polymarket.com 手动管理。

### 6.2 立刻停 bot **且**全平仓
```bash
# 正常 SIGINT，bot 会先 cancel open orders 再 SELL FOK @ 0.01
Ctrl-C   # 或 kill -2 <pid>
```
日志会有 `LIVE: triggering emergency_close_all on N open position(s)`。FOK 失败的话日志会打 error，仓位仍残留——去 UI 平。

### 6.3 怀疑被攻击 / 私钥泄漏
1. **立刻**去 polymarket.com → Profile → Disable API key（吊销现有 L2 三件套）
2. **立刻**把 vault 资金 withdraw 到新地址
3. 不要再用旧 EOA / proxy；走 §3.2 重新生成 keystore + onboarding

---

## 7. 与 step 工作流配合

每改一处都要走：**代码改 → `docs/steps/step-<N>-report.md`（参考 TEMPLATE.md）→ 等 user review → `git commit` → 更新 memory → 下一步**（CLAUDE.md 末尾 + DECISIONS.md W1）。
跳步不被接受——历史 36 份 step 报告就是这条规则的产物。
