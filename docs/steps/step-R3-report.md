# Step R3 — 链上读模块（USDC 余额 + allowance）

## 概要

实盘下单链路第三步：让 bot 启动期能查 Polygon 链上的 USDC 余额和对 CTFExchange 的 allowance，给后续 R6（approval 校验）打地基。read-only RPC，零写操作，无金额风险。

## 关键命令

```bash
cmake --build build -j

# 直接测 ChainClient（独立于 wallet，无需密码）
./build/test_chain                                # 默认查用户 EOA
./build/test_chain 0x356E4c0a...                  # 查 Polymarket proxy
./build/test_chain 0xA4D94019934D8333Ef880ABFFbF2FDd611C762BD  # Aave vault（非零数据验证）

# live 模式端到端：钱包 → 链上读 → 卡 R4
KEYSTORE_PASSWORD='你的密码' ./build/polymarket-arb config/config.json
```

## 文件清单

| 操作 | 文件 |
|------|------|
| 修改 | `src/utils/config.h`（PolymarketConfig 加 proxy_address/api_address；PolygonConfig 重写）|
| 修改 | `src/utils/config.cpp`（加载 polygon 块 + RPC 默认值兜底）|
| 新增 | `src/net/chain_client.h` `src/net/chain_client.cpp` |
| 修改 | `src/core/live_trader.cpp`（实现 `read_chain_state`）|
| 修改 | `src/main.cpp`（live 守卫：R2 后跑 R3，再卡 R4）|
| 修改 | `CMakeLists.txt`（chain_client.cpp 加入 SOURCES + 新增 test_chain 可执行）|
| 新增 | `tests/test_chain.cpp` |
| 修改 | `config/config.example.json`（同步 polymarket / polygon 字段）|
| 新增 | `docs/steps/step-R3-report.md` |

## 设计决策

### 1. 为什么 ChainClient 复用 net::HttpClient？

项目已经有 boost::beast 写的 HTTPS HttpClient，支持 POST + 代理 + 超时。RPC 走 HTTPS POST + JSON body，重新写一遍是浪费。直接调 `http_.post(url, body, headers)`。

### 2. 多 RPC failover

公开免费 Polygon RPC 经常挂（之前测试时 polygon-rpc.com 直接 403）。默认配 3 个：
- `polygon-bor-rpc.publicnode.com`
- `polygon.drpc.org`
- `1rpc.io/matic`

调用按顺序尝试，遇 4xx/5xx/timeout 切下一个。**找到能用的会把它升为下次首选**（atomic counter 记录），避免每次都从头试。

### 3. ABI encode 自己写，不引依赖

只需要：
- `balanceOf(address)`：selector + 32 字节地址
- `allowance(address,address)`：selector + 两个 32 字节地址

总共 60 行手工实现。引第三方 ABI 库（如 web3.cpp）会引一堆 bigint 依赖。

### 4. uint256 解码：手工 hex→dec 大数除法

USDC 6 位精度时，余额 9.2e12 USDC 就溢出 int64。Polymarket 用户余额远不会到这个量级，但 Aave 池可能。`decode_uint256` 用 nibble 数组做大数除法 → string，无外部依赖；`decode_usdc6` 在 fits-int64 时走 stoll 快路径，溢出走慢路径。

### 5. 合约地址硬编码默认值，不强制配置

Polymarket 主网 USDC.e / USDC native / CTF / CTFExchange 地址都是稳定值（多年没变），写在 `PolygonConfig` struct 默认值里。配置文件可覆盖（用于 testnet / 异常情况），但 100% 用户不需要自填。

### 6. CTF balanceOf 不在 R3 实现

理由：CTF (ERC-1155) 的 `balanceOf(address, uint256)` 需要知道 token_id。bot 当前空仓，没有 token_id 可查。R7 下单后 Position 持有 token_id，那时再查 CTF 余额对账。

## 红线遵守

- [x] 没有写交易（read-only RPC）
- [x] 没有跳过错误处理（所有 RPC 调用失败要么 failover 要么抛）
- [x] 编译通过，无 warning
- [x] dry_run 行为零变化
- [x] 余额单位明确（USDC = 6 位精度，函数名带 `_usdc6` 标注）

## 验收结果

**ChainClient 单元 + 集成测试：**
```
$ ./build/test_chain 0x356E4c0a80B5Ac2466B7a62A11B35e8DbC7d2196
[ERC-20 balanceOf]
  USDC.e    balance: $0
  USDC.nat  balance: $0
[ERC-20 allowance(addr, CTFExchange)]
  USDC.e    allowance: $0
  USDC.nat  allowance: $0
[Native balance]
  MATIC raw:  0x0
  MATIC dec:  0 wei
[Decode unit tests]
  decode 0x000...000 → 0 ✓
  decode 0x100 → 256 ✓
  decode 0xf4240 → 1000000 ✓
  decode_usdc6(0xf4240) → 1.0 ✓
```

**非零数据验证（Aave V3 USDC vault）：**
```
$ ./build/test_chain 0xA4D94019934D8333Ef880ABFFbF2FDd611C762BD
  USDC.nat  balance: $1.28882e+07   ← $12.88M，链上确认
```

**Live mode 端到端（守卫渐进）：**
- R2 wallet 解密 OK → R3 chain read OK → R4 仍卡
- 错误密码 → R2 失败，return 1
- proxy_address 空 → R3 报「polymarket.proxy_address is empty」+ return 1

## 遗留问题

1. **CTF balanceOf** 暂未实现（R7+ 在持仓时再加）
2. **MATIC 余额**目前只 raw / wei 显示，没转成 ether 浮点（因为 ERC-4337 + EIP-7702 用户其实不需要 MATIC，gas 走 Polymarket relayer）
3. 用户 USDC 余额仍为 0，没充过钱。下一步可以选：
   - **充 $1 测试** → R3 会查到非零，更直观
   - **直接进 R4 EIP-712** → 不依赖资金，可纯码

下一步 R4：EIP-712 签名核心模块（自实现，secp256k1 + keccak256 + RLP-like type hash）。
