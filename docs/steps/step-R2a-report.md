# Step R2a — 密码学地基（Keccak-256 + Web3 keystore）

## 概要

落地实盘下单链路的密码学底座：Keccak-256（Ethereum 用，**不是** SHA3-256）+ Web3 keystore v3 加密/解密 + EIP-55 地址派生。所有原语自实现 / 走 OpenSSL，零新依赖。20 个单元测试全过。本步**不集成进 bot 主流程**，只在测试和未来 R2b 的导入工具里被调用。

## 关键命令

```bash
cmake -B build && cmake --build build -j   # 编译（新增 polymarket_crypto.a + test_crypto）
./build/test_crypto                         # 跑单元测试，期望「20 passed, 0 failed」
```

## 文件清单

| 操作 | 文件 |
|------|------|
| 新增 | `src/crypto/keccak256.h` `src/crypto/keccak256.cpp` |
| 新增 | `src/crypto/wallet.h` `src/crypto/wallet.cpp` |
| 新增 | `tests/test_crypto.cpp` |
| 修改 | `CMakeLists.txt`（新增 `polymarket_crypto` 静态库 + `test_crypto` 可执行）|
| 新增 | `docs/steps/step-R2a-report.md` |

## 设计决策

### 1. 为什么自己写 Keccak 而不是用 OpenSSL EVP_sha3_256？

**Keccak ≠ SHA3**。两者算法核心一样（KECCAK-p[1600, 24]），但**padding domain byte 不同**：SHA3 用 `0x06`，原版 Keccak 用 `0x01`。Ethereum 在 SHA3 标准化前就锁定了原版 Keccak，所以一字之差。OpenSSL 只提供 `EVP_sha3_256`（即标准 SHA3），用它会算出**完全错的哈希**。

实现走 FIPS 202 + 公有领域参考（150 行），无 unsafe 操作，纯 `uint64_t` 数组运算。

### 2. PrivateKey 包装类（RAII + 删除拷贝）

私钥泄露最常见来源是「忘了清擦栈/堆上的明文字节」。`PrivateKey`：
- 删除拷贝构造/赋值（防止意外扩散）
- 移动构造/赋值会清擦源对象
- 析构强制 `secure_zero`（用 `volatile` 函数指针绕过编译器优化）

调用者只能通过 `data()` 拿到 `const uint8_t*`，且生命周期与 `PrivateKey` 实例绑定。

### 3. scrypt 参数选 N=2^18 而不是更高

go-ethereum 默认就是 `N=2^18, r=8, p=1`（约 256 MB 内存、~1s on M1）。MetaMask / MyEtherWallet / Polymarket 导出都是这个参数。我们如果用 `N=2^20` 会更安全但与生态不兼容（导出/导入时要协商参数）。

OpenSSL scrypt 默认有 1GB 内存上限，`N=2^18` 占 ~256 MB，但保险起见显式抬到 2GB（`OSSL_KDF_PARAM_SCRYPT_MAXMEM`）。

### 4. MAC 验证用常时比较

`constant_time_eq`（XOR 累积 + 全 0 判定）防 timing attack。虽然 keystore 在本地解密、攻击面小，但养成习惯。

### 5. 测试不依赖外部框架

避免引入 GoogleTest 等大依赖。手写 `ASSERT_EQ` / `ASSERT_TRUE` 宏，单文件可执行，跑了打绿色 `PASS` / 红色 `FAIL` + summary。

## 测试覆盖

```
[test_hex_helpers]      4 cases （encode/decode/异常）
[test_keccak256]        7 cases （空串、"abc"、quick brown fox、135/136/137/200×'a' 边界）
[test_address_derivation]  3 cases （privkey=1/2/3 → 公认 KAT 地址）
[test_keystore_roundtrip]  6 cases （JSON 结构、roundtrip、错误密码 MAC mismatch）
合计 20 cases，全过。
```

KAT 来源：
- Keccak-256 边界长度由 `pycryptodome` 生成对照
- 地址派生 KAT (privkey=1/2/3) 来自 [eth-private-key-to-address 公开测试向量](https://privatekeyfinder.io/private-keys/ethereum/1)
- keystore 用自加密自解密的 roundtrip + 错误密码 MAC 检查

## 红线遵守

- [x] 没有引入硬编码密钥（所有测试用 `0x00...01/02/03` 这种公开测试向量）
- [x] 没有跳过错误处理（所有 OpenSSL/secp256k1 调用都检查返回值并抛异常）
- [x] 编译通过，无 warning
- [x] 私钥处理：RAII + 删除拷贝 + 析构强制清擦 + MAC 常时比较

## 验收结果

```
===== crypto unit tests =====

[test_hex_helpers]      4 PASS
[test_keccak256]        7 PASS
[test_address_derivation]  3 PASS
[test_keystore_roundtrip]  6 PASS

===== 20 passed, 0 failed =====
```

## 遗留问题

无。R2b 会消费本步的 API：
- `tools/import_key.cpp` 用 `encrypt_keystore` 把 `/tmp/pk.txt` 加密成 `keystore.enc`
- `LiveTrader::init_wallet()` 用 `decrypt_keystore` + `derive_address` 加载并自检
