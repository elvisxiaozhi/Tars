# Step 2.7a — 修复到期持仓阻塞 bug

## 问题

市场到期后被 `fetch_from_gamma` 清理，`get_market()` 返回 nullptr，持仓管理代码 `continue` 跳过。导致：
- 持仓永远不会被关闭
- `open_positions_` 一直为 1
- 单仓制下新单永远无法开仓

## 修复

当 `get_market()` 返回 nullptr 时，按最后已知价格强制平仓：
- 计算剩余仓位的 P&L
- 标记 `pos.closed = true`，`close_reason = "expired"`
- 调用 `risk.remove_position()` 释放仓位
- 记录到 `trades.jsonl`

## 文件清单

| 操作 | 文件 |
|------|------|
| 修改 | `src/main.cpp` — 持仓管理增加到期结算逻辑 |

## 验收结果

```
cmake --build build   # 编译通过，零 warning
```
