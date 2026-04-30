# Step R6 — Approval 校验

## 概要

实盘下单链路第六步：启动期校验 proxy 对 CTFExchange 的 USDC allowance ≥ 最大单笔交易额，
不足时打印操作指引并阻止 bot 启动。纯链上读，零写操作，零金额风险。

## 关键命令

```bash
cmake --build build -j

KEYSTORE_PASSWORD='你的密码' ./build/polymarket-arb config/config.json
# allowance=0 时预期输出：
#   APPROVAL: allowance $0.0000 < required $100.00
#   proxy address  : 0x356E4c0a...
#   CTFExchange    : 0x4bFb41d5...
#   操作方式（任选一）：...
```

## 文件清单

| 操作 | 文件 |
|------|------|
| 修改 | `src/core/live_trader.cpp`（实现 check_approvals_sufficient）|
| 修改 | `src/main.cpp`（R6 守卫步骤）|
| 新增 | `docs/steps/step-R6-report.md` |

## 设计决策

### 1. 复用 read_chain_state() 拿最新 allowance

`check_approvals_sufficient` 内部直接调 `read_chain_state()`，获取链上最新快照。
这意味着启动期会多一次 RPC batch（R3 守卫已读过一次），但保证数据新鲜，避免缓存问题。

### 2. bot 不自动 approve，只检查

自动 approve 需要构造 + 签名 + 广播真实链上交易，引入私钥直接控制 proxy 资产的风险。
决策维持原设计：bot 只验，不操作；失败时打印清晰的手动操作路径。

### 3. 阈值 = `risk.max_single_trade`

用配置中最大单笔金额（默认 $100）作为 allowance 下限，保证下最大单时不被 allowance 卡住。

## allowance 来源说明

Polymarket proxy（Simple7702Account）在 Polymarket 网页 **Deposit 充 USDC 时会自动设置
allowance**（Polymarket 的前端在充值流程里包含了一步 `approve(CTFExchange, MAX_UINT)`）。
所以用户只需充值，allowance 就会变成 MAX_UINT，R6 自动通过。

## 红线遵守

- [x] 无链上写操作
- [x] 编译通过，无 warning
- [x] dry_run 行为零变化

下一步 R7：入场限价单 `place_entry_order`（首次实盘写操作，高风险，先做完整设计再动代码）。
