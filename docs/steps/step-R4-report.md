# Step R4 — EIP-712 签名核心模块

## 概要

实盘下单链路第四步：实现 EIP-712 typed structured data 签名，为 R5（CLOB API 鉴权）和 R7（Order 签名下单）打地基。纯密码库，零网络调用，无金额风险。

## 关键命令

```bash
cmake --build build -j
./build/test_eip712          # 全量 KAT（11 个检查）
./build/test_crypto          # 原有 Keccak/wallet KAT 回归
```

## 文件清单

| 操作 | 文件 |
|------|------|
| 新增 | `src/crypto/eip712.h` |
| 新增 | `src/crypto/eip712.cpp` |
| 新增 | `src/core/polymarket_types.h` |
| 新增 | `src/core/polymarket_types.cpp` |
| 新增 | `tests/test_eip712.cpp` |
| 修改 | `CMakeLists.txt`（eip712.cpp 加入 polymarket_crypto；polymarket_types.cpp 加入 SOURCES + test_eip712 target）|
| 修改 | `src/main.cpp`（守卫消息更新：R4 完成，卡 R5）|
| 新增 | `docs/steps/step-R4-report.md` |

## 设计决策

### 1. 分层 API：低层 encode_* + 高层 Polymarket helpers

底层（`crypto/eip712.h`）：每个函数返回一个 32 字节 ABI 词，调用者手工组装 struct hash。
上层（`core/polymarket_types.h`）：`sign_poly_order` + `sign_clob_auth`，把 Polymarket 特定类型映射到底层调用，调用者不需要知道任何 EIP-712 细节。

### 2. 嵌套 struct 处理

EIP-712 规定：嵌套 struct 字段的编码值 = 其 `hashStruct()`（32 字节）。API 设计让这一点透明：`eip712_struct_hash` 接受 `vector<array<uint8_t,32>>`，内层 struct 的 hash 直接传进去即可，无需额外包装。Mail 示例的 Person 嵌套测试验证了这一点。

### 3. 完整 type string（含依赖类型）

EIP-712 typeHash 要求 primary type + 按字母序排列的所有引用类型拼接。对 Polymarket 的 Order 和 ClobAuth 来说没有嵌套 struct，type string 就是单一定义，简单。对通用测试（Mail），调用者手工提供完整 full_type_string：`"Mail(Person from,…)Person(string name,…)"`。

### 4. 任意精度 uint256（eip712_uint256_str）

Polymarket binary outcome token ID 是大 uint256（~ 78 位十进制），超 uint64 范围。用 byte 数组乘法逐字符处理，复杂度 O(n × 32)，无外部 bignum 依赖。

### 5. secp256k1 recoverable signature，v = recovery_id + 27

使用 `secp256k1_ecdsa_sign_recoverable`（需 `secp256k1_recovery.h`，brew secp256k1@0.7.1 包含）。libsecp256k1 默认保证 low-s（s ≤ n/2），v = 0 或 1，EIP-2 标准加 27 得 27 或 28。signing_ctx() 静态单例，避免每次调用创建/销毁。

### 6. ClobAuthDomain 没有 verifyingContract

`eip712_domain_separator` 支持 `verifying_contract` 为空：为空时从 type string 中省略该字段（type string 变短，影响 domain type hash）。这正是 Polymarket ClobAuth domain 的用法。

## 验收结果

```
=== test_eip712 ===

[1] Encoding primitives
  [OK]   uint256(0)
  [OK]   uint256(1)
  [OK]   uint256(0xDEADBEEF)
  [OK]   uint256_str("0")
  [OK]   uint256_str("256")
  [OK]   uint256_str("1000000") = 0xf4240
  [info] uint256_str(large tokenId): 7337a8d8544068af6aaf6531a97e7988838fd8a4fe401eef71f043216fa4fc1a
  [OK]   address(0xCcCC…) left-padded
  [OK]   bool(true)
  [OK]   bool(false)

[2] EIP-712 Mail example (reference: EIP-712 proposal)
  [info] Person typeHash: b9d8c78acf9b987311de6c7b45bb6a9c8e1bf361fa7fd3467a2163f994c79500
  [info] Mail typeHash: a0cedeb2dc280ba39b857546d74f5549c3a1d7bdc2dd96bf881f76108e23dac2
  [OK]   domainSeparator     = 0xf2cee375fa42b42143804025fc449deafd50cc031ca257e0b194a650a912090f
  [info] hashStruct(from): fc71e5fa27ff56c350aa531bc129ebdf613b772b6604664f5d8dbe21b85eb0c8
  [info] hashStruct(to): cd54f074a4af31b4411ff6a60c9719dbd559c221c8ac3492d9d872b041d703d1
  [OK]   hashStruct(mail)    = 0xc52c0ee5d84264471806290a3f2c4cecfc5490626bf912d01f240d7a274b371e
  [OK]   final digest (Mail) = 0xbe609aee343fb3c4b28e1df9e632fca64fcfaede20f02e86244efddf30957bd2

[3] ECDSA sign — v = 28 (27 or 28) ✓

[4] Polymarket 类型 hash（待 R5 用 py-clob-client 交叉验证）
  Order typeHash:                  0xb144ee86fcc739476311b60cb7484719a4f4ef43eee8375714ace3054ce701b5
  ClobAuth typeHash:               0x3d2ff07a1e830c4aa765ed45b680aa39d5721165d13724f68b7543c29a04979e
  CTFExchange domain separator:    0x1a573e3617c78403b5b4b892827992f027b03d4eaf570048b8ee8cdd84d151be
  ClobAuthDomain separator:        0xcfc66be2a3b30464cb3b588324101f660c9a205fa76e8e5f83ee16a528e1c4cb

[5] sign_poly_order smoke test — v = 28 ✓

=== ALL PASS ===
```

## 待 R5 验证的关键值

R5 实现 CLOB API 鉴权时，必须用 py-clob-client 对照验证：
- `ClobAuth typeHash` 是否匹配（type string 中 address 字段类型可能是 `string` 而非 `address`）
- `ClobAuthDomain separator`（domain name / version 是否一致）
- 实际签名是否被服务器接受

如果验证失败，需回头修改 `CLOB_AUTH_TYPE` 字符串或 domain 定义。

## 红线遵守

- [x] 无网络调用（纯密码库）
- [x] 私钥不出现在日志或错误信息
- [x] 编译通过，无 warning
- [x] dry_run 行为零变化
- [x] KAT 与权威参考值（EIP-712 proposal Mail example）完全吻合

## 遗留

- ClobAuth type string 中 `address` 字段类型（`address` vs `string`）待 R5 实测确认
- Polymarket 类型 hash 待 py-clob-client 交叉验证

下一步 R5：CLOB API key 鉴权（用 sign_clob_auth 打 POST /auth/api-key，拿 api_key + secret + passphrase）。
