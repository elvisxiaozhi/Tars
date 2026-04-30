# Step R5 — CLOB API Key 鉴权

## 概要

实盘下单链路第五步：用 R4 的 EIP-712 签名向 Polymarket CLOB 服务器请求 API 凭证
（`apiKey` / `secret` / `passphrase`），供 R7/R8 下单时的 HMAC-SHA256 请求鉴权使用。
无链上写操作，风险极低。

## 关键命令

```bash
cmake --build build -j

# 端到端验证（需 KEYSTORE_PASSWORD + config.json 配置了 proxy_address）
KEYSTORE_PASSWORD='你的密码' ./build/polymarket-arb config/config.json
# 期望：R2 OK → R3 OK → R5 OK → 卡 R6 守卫打印
```

## 文件清单

| 操作 | 文件 |
|------|------|
| 修改 | `src/core/live_trader.h`（加 `#include crypto/wallet.h`；私钥/凭证成员）|
| 修改 | `src/core/live_trader.cpp`（init_wallet 保留私钥；实现 ensure_clob_authenticated）|
| 修改 | `src/main.cpp`（R5 守卫步骤）|
| 新增 | `docs/steps/step-R5-report.md` |

## 设计决策

### 1. 私钥跨方法生命周期

原来 `init_wallet()` 里 `PrivateKey key` 是局部变量，函数返回即析构。
R5/R7/R8 都需要用它签名，所以改成 `key_ = std::move(key)` 存入成员。
`PrivateKey` 是 RAII 类（析构自动 secure_zero），不新增安全风险。

### 2. GET 优先，404/401 降级 POST

- `GET /auth/api-key`：derive 现有 key，**幂等**，推荐日常使用（每次启动拿同一套凭证）
- `POST /auth/api-key`：create new key（首次或 key 被撤销时）
- bot 启动时先 GET，收到 404/401 再 POST，逻辑干净

### 3. API key 不持久化

凭证仅存内存（`clob_api_key_` / `clob_secret_` / `clob_passphrase_`），每次启动重新 derive。
好处：无凭证文件泄露风险，简单可靠。
代价：每次启动多一次 HTTPS 请求（< 200 ms）。

### 4. 鉴权失败提示

HTTP 401 时额外打印提示：「检查 ClobAuth type string 中 address 字段类型」。
这是 R5 最可能出问题的地方（`address` vs `string` 类型，影响 typeHash）。

## 私钥 / 凭证在 LiveTrader 内部布局

```
wallet_initialized_ + key_ + address_       ← R2 init_wallet()
clob_authenticated_ + clob_api_key_/secret_/passphrase_  ← R5 ensure_clob_authenticated()
```

R7/R8 直接访问这些私有成员（同类，无需 getter）。

## 待验证

1. **ClobAuth type string 是否正确**：当前用
   `"ClobAuth(address address,uint256 timestamp,uint256 nonce,string message)"`
   如果服务器返回 401，需检查 py-clob-client 里 `CLOB_AUTH_STRUCTURE` 的 address 字段类型
   是 `"address"` 还是 `"string"`，修改 `polymarket_types.cpp` 的 `CLOB_AUTH_TYPE`。

2. **GET vs POST**：若 GET 始终返回非 200，改用 POST（把 GET 注释掉）。

3. **字段名大小写**：服务器返回 `"apiKey"` 还是 `"api_key"` —— 代码用 `j.at("apiKey")`，
   如果字段名不对会抛 `json::out_of_range`，看错误信息直接改。

## 红线遵守

- [x] 无链上写操作
- [x] 私钥不出现在日志（只打 api_key 前 8 字符）
- [x] 编译通过，无 warning
- [x] dry_run 行为零变化

下一步 R6：Approval 校验（链上读 allowance ≥ 阈值，不足则报错引导用户手动 approve）。
