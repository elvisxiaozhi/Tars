# Step 2.5f — 禁用 dry_run 阶段熔断机制

## 概要

dry_run 测试阶段暂时禁用连亏熔断和日亏熔断，以便收集更多测试数据。live 模式必须重新启用。

## 改动详情

- `can_open_position()` 中移除 `killed_` 和 `consecutive_losses_` 检查
- `record_loss()` 中移除 `killed_ = true` 的赋值逻辑，保留日志记录
- `strategy-btc-1h.md` §九 标注 dry_run 禁用、live 必须启用

## 文件清单

| 操作 | 文件 |
|------|------|
| 修改 | `src/core/risk_manager.cpp` — 禁用熔断检查和触发 |
| 修改 | `docs/strategy-btc-1h.md` — §九.3/§九.4 标注状态 |

## TODO（live 模式前必须完成）

- [ ] 重新启用连亏 3 次熔断
- [ ] 重新启用日亏 5% 熔断
- [ ] 添加每小时自动重置机制（每个新 K线 独立计数）

## 验收结果

```
cmake --build build   # 编译通过，零 warning
```
