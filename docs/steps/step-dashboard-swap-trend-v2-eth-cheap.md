# Step dashboard-swap — trend_v2 升独立 tab / eth_cheap_v1 降入 Finance

## 概要

调整 dashboard 布局:把 `trend_v2`(当前表现最优的实验)从 Finance 标签页末尾的子面板，提升为独立顶级标签页;把 `eth_cheap_v1`(持续亏损待退役)从独立标签页降级到 Finance 标签页底部。纯布局 + 标签文案改动，策略逻辑、API、element ID、JS 数据绑定均未变。

## 关键命令

```bash
# 仅前端(内嵌于 C++ header)，需整体重编后由 manager 重启 bot 生效
cmake --build build -j
# 验证结构(无需编译)
grep -nE 'data-tab=|id="(experiment|trend-v2-experiment)-details"' src/dashboard.h
```

## 文件清单

| 操作 | 文件 |
|------|------|
| 修改 | `src/dashboard.h` |
| 新增 | `docs/steps/step-dashboard-swap-trend-v2-eth-cheap.md` |

## 改动明细（5 处，均在 src/dashboard.h）

1. tab 按钮 `data-tab="regime"` 文案 `ETH Cheap` → `Trend Follow v2`（`data-tab`/`id` 不变，保 JS 与 localStorage）
2. `regime-rules-details` 规则卡内容 ETH Cheap → Trend Follow v2（BTC/ETH/BNB, 41-45min, dev≥0.18%, TP0/1/2, trailing）
3. `tab-regime` 数据面板块 `experiment-details`(exp-*) ↔ Finance 底部 `trend-v2-experiment-details`(trend-v2-exp-*) 整块对调
4. Finance 规则网格里 `Trend Follow v2` 卡 → `ETH Cheap` 卡
5. （3 的另一半）Finance 底部现为 `experiment-details`(eth_cheap)

## 设计决策

- 选「整块搬 + 保留所有 element ID」而不是「改 ID + 改 JS」：`renderExperiment(prefix,...)` 与 `persistDetails(id,...)` 全靠 ID 绑定，块移动时 ID 跟着移动，JS 零改动、回归面最小。
- `data-tab`/`id="tab-regime"` 保持不变：tab 切换 JS 与用户 localStorage 偏好(`dashboard.scope` 等)不受影响；仅改可见按钮文案（用户已确认该选项必然带这一文案改动）。
- 未改 `strategy_name`/变量名/jsonl 文件名/分发路由（用户明确「先不改名」）。

## 红线遵守

- [x] 没有引入硬编码密钥
- [x] 没有跳过错误处理（无逻辑改动）
- [ ] 编译通过，无 warning —— 见遗留

## 验收结果

```
结构静态校验通过:
- tab 按钮: main / Trend Follow / Trend Follow v2 / Finance
- 唯一 ID 各 1 次: experiment-details, trend-v2-experiment-details,
  regime-rules-details, exp-balance, trend-v2-exp-balance
- tab-regime  = Strategy Rules — Trend Follow v2 + Trend Follow v2 Experiment
- tab-finance = 规则网格(含 ETH Cheap 卡) + Finance/4H/Daily + ETH Cheap Experiment
- renderExperiment('exp'...) / ('trend-v2-exp'...) 未改
```

## 遗留问题

- 未本地编译/浏览器实测:dashboard 内嵌于 C++ 二进制，运行实例在远程服务器(159.203.56.165:9090)。需 `cmake --build` 后由 manager 重启 bot 才会生效。建议 review 后在服务器重编验证渲染。
- 这是纯展示层调整;`eth_cheap_v1` 退役、`trend_v2` 是否升主仍是未决策略问题（见对话分析，待样本 ≥50 + 升主阈值确定）。
