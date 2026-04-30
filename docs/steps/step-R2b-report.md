# Step R2b — 钱包导入 CLI + LiveTrader::init_wallet 集成

## 概要

把 R2a 的密码学库消费起来：写一个一次性 CLI 工具 `import_key`（明文私钥 → keystore.enc），实现 `LiveTrader::init_wallet()`（启动期解密 + 派生地址 + 比对 cfg）。Live 模式启动守卫渐进放宽：现在能过钱包阶段，仍卡在 R3。

## 关键命令

```bash
# 编译（新增 import_key 可执行）
cmake --build build -j

# 用法演示（你的真实流程见「使用步骤」章节）
./build/import_key /tmp/pk.txt ./keystore.enc 0x356E4c0a80B5Ac2466B7a62A11B35e8DbC7d2196

# 启动 live 模式（验证钱包阶段）
KEYSTORE_PASSWORD='你的密码' ./build/polymarket-arb config/config.json
```

## 文件清单

| 操作 | 文件 |
|------|------|
| 新增 | `tools/import_key.cpp` |
| 修改 | `src/crypto/wallet.h` `src/crypto/wallet.cpp`（加 `read_password`：termios 关回显）|
| 修改 | `src/core/live_trader.cpp`（实现 `init_wallet()` + `wallet_address()`）|
| 修改 | `src/main.cpp`（加 `#include core/live_trader.h`；live 守卫加 R2 钱包自检）|
| 修改 | `CMakeLists.txt`（新增 `import_key` 可执行）|
| 新增 | `docs/steps/step-R2b-report.md` |

## 设计决策

### 1. 私钥从文件读，不接受命令行参数

`./import_key /tmp/pk.txt ...` 是文件路径，不是私钥本身。理由：命令行参数会出现在 `ps aux`、shell history、各种监控/审计日志里。用文件 + 0600 权限至少把暴露面缩到本机磁盘。

### 2. 密码二来源：env var 优先，stdin fallback

- **`KEYSTORE_PASSWORD` env var**：自动化场景（systemd / docker / cron）必须，stdin 没有 TTY 时也能跑
- **stdin 交互式**：手动启动的本地调试默认走这条路，无回显防偷窥

按顺序：env var 非空就用；否则提示输入。

### 3. read_password 用 termios 关 ECHO

`getpass(3)` 是 POSIX 的标准函数但 macOS 用之 deprecated。直接 `tcsetattr` + `~ECHO` 简单且无 deprecate 警告。如果 stdin 不是 TTY（如 pipe），则跳过 termios 操作直接读，方便测试 + CI。

### 4. 地址比对 case-insensitive

cfg 里写的地址可能是小写、可能是 EIP-55 大小写。两边都拉成小写比较，避免「明明对的但大小写不一致被拒」的坑。

### 5. import_key 强制要求 keystore_out 不存在

避免误覆盖已有 keystore（你可能有多个钱包）。需要重新生成时手动删掉旧文件。

### 6. password 临时保存策略

import_key 的 `pwd1` 在两次提示和加密期间存活 3 个时间点，加密完立即 `secure_zero`。`init_wallet` 只用一次解密，立即 zero。这是**必要妥协**：std::string 内存清擦不是绝对可靠（SSO + COW + 移动语义都可能留下副本），但比啥都不做强。

## 使用步骤（你的实操）

确认 `/tmp/pk.txt` 已就绪、内容为你的 64 字符 hex 私钥后：

```bash
# 1. 导入私钥（系统会要求设置 keystore 密码两次）
./build/import_key /tmp/pk.txt ./keystore.enc 0x356E4c0a80B5Ac2466B7a62A11B35e8DbC7d2196

# 2. 销毁明文私钥（macOS）
rm -P /tmp/pk.txt

# 3. 清 shell history
history -c && history -w

# 4. 更新 config/config.json（已有正确的 keystore_path 和 address 即可，import_key 输出会提示）

# 5. 验证：live 模式启动到钱包阶段
KEYSTORE_PASSWORD='你设置的密码' ./build/polymarket-arb config/config.json
# 预期看到：
#   init_wallet: OK, address=0x356E4c0a...
#   LIVE MODE 部分就绪（进度：R2 wallet OK）
#   待完成：R3 链上读 / ...
```

## 红线遵守

- [x] 私钥永不出现在命令行 / 日志（只输出地址）
- [x] 密码 stdin 无回显，env var 仅用于自动化
- [x] keystore.enc chmod 0600，import_key 拒绝覆盖已有文件
- [x] 编译通过，无 warning
- [x] dry_run 行为零变化

## 验收结果

**单元测试（R2a 仍全过）：**
```
20 passed, 0 failed
```

**端到端 smoke test（使用公开测试 privkey=1）：**
```
$ printf "test-password-12345\ntest-password-12345\n" | ./import_key /tmp/test_pk.txt /tmp/test_keystore.enc 0x7E5F4552091A69125d5DfCb7b8C2659029395Bdf
Derived address: 0x7E5F4552091A69125d5DfCb7b8C2659029395Bdf
✓ Matches expected address (case-insensitive)
Encrypting (scrypt N=262144, ~1s)... done.
Wrote /tmp/test_keystore.enc (0600)

$ KEYSTORE_PASSWORD=test-password-12345 ./polymarket-arb /tmp/cfg-live-test.json
[info] init_wallet: using password from KEYSTORE_PASSWORD env var
[info] init_wallet: OK, address=0x7E5F4552091A69125d5DfCb7b8C2659029395Bdf
[error] LIVE MODE 部分就绪（进度：R2 wallet OK，address=0x7E5F4552...）
[error] 待完成：R3 链上读 / ...
exit code: 1
```

**边缘情况：**
- ✅ 错误密码 → `wallet init failed: invalid password (MAC mismatch)` + return 1
- ✅ 配置地址不匹配 → `derived address does NOT match cfg.wallet.address (expected ..., got ...)` + return 1
- ✅ dry_run 不受影响（启动正常，BTC 行情正常）

## 遗留问题

无。下一步 R3：链上读模块（USDC 余额 / CTF 余额 / allowance）。
需要在 `live_trader.cpp` 加 Polygon JSON-RPC 客户端 + ABI encode 三个 `eth_call`。
