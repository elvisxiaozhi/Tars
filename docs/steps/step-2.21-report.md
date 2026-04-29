# Step 2.21 — 合并 origin/main + 移除 BTC dev 0.15-0.50% 甜区过滤

## 概要

将远端 `origin/main` 的 9 个 commit（Step 2.15–2.20 改进）合并到本地 `feature` 分支，但移除其中**最具争议**的 BTC 偏差甜区过滤——4-28 实盘数据显示该过滤会误杀低 dev 区的赢家（P3 +1.10、P4 +3.29）。保留死水早退、真实费率、低 trailing 阈值等无争议改进。

## 关键命令

```bash
# 合并
git fetch origin
git merge origin/main --no-edit

# 编辑两处源码（见下「文件清单」）

# 编译运行
cmake --build build -j && ./build/polymarket-arb
```

## 文件清单

| 操作 | 文件 | 说明 |
|------|------|------|
| 修改 | `src/core/strategy.cpp` | 删除 `evaluate_entry` 里的 BTC dev 0.15-0.50% 过滤块（10 行）|
| 修改 | `src/main.cpp` | 简化轮询：删除 `abs_dev < 0.10` 深空闲 45s 分支，回退到统一 15s |

## 设计决策

### 1. 为什么不直接用 origin/main？

origin/main 的 BTC dev 甜区过滤依据是 38 笔历史回测，但 4-28 实盘 8 笔数据中：

| dev 区间 | 回测结论 (38 笔) | 4-28 实测 |
|----------|------------------|-----------|
| 0-0.15% | 18% WR / -$6.48 | 50% WR / +$0.89 (4 笔) |
| ≥0.15%  | 21-40% WR | — |

样本量太小不能下定论，但**两笔最大利润 P3/P4 都在低 dev 区**，盲目套用甜区过滤会把这两笔过滤掉，净效果不一定优。决定先放开过滤，跑几天再看。

### 2. 为什么也删了深空闲 45s 分支？

origin/main 里 `abs_dev < 0.10 → 45s` 是配套甜区过滤的节流——「反正不入场，慢慢轮询」。移除过滤后，低 dev 也是有效信号区，45s 间隔会错过 P3 这种入场后 7 分钟就触发 TP 的快速行情。统一回退 15s。

### 3. 为什么不用 `git revert 924bc26`？

该 commit 同时包含「BTC 偏差过滤」和「仓位 10→17 shares」两个改动，后续 `f632529` 又把仓位改回 10。直接 revert 会触发冲突或语义错乱。用源码 Edit 干净精确。

## 红线遵守

- [x] 没有引入硬编码密钥
- [x] 没有跳过错误处理
- [x] 编译通过，无 warning

## 验收结果

```
[ 47%] Building CXX object CMakeFiles/polymarket-arb.dir/src/main.cpp.o
[ 52%] Building CXX object CMakeFiles/polymarket-arb.dir/src/core/strategy.cpp.o
[ 78%] Linking CXX executable polymarket-arb
[100%] Built target polymarket-arb
```

合并 stat：

```
16 files changed, 640 insertions(+), 48 deletions(-)
src/core/strategy.cpp  +45（再 −10 删除甜区块）
src/main.cpp          +100（再 −4 简化轮询）
```

## 4-28 数据预期对比

| 笔 | feature 实际 | origin/main 完整 | **本步策略（main−过滤）** |
|----|-------------|------------------|---------------------------|
| P1 (dev=0.34) | +1.40 | +1.40 | +1.40 |
| P2 (dev=0.27) | -1.34 | ~0 (保本档) | ~0 |
| P3 (dev=0.08) | +1.10 | 0 (过滤) | **+1.10** ✓ |
| P4 (dev=0.14) | +3.29 | 0 (过滤) | **+3.29** ✓ |
| P5 (dev=0.12) | -1.66 | 0 (过滤) | -1.66 (秒亏，无可救)|
| P6 (dev=0.08) | -1.76 | 0 (过滤) | -1.76 (秒亏，无可救)|
| P7 (dev=0.06) | -0.78 | 0 (过滤) | 改善（+15¢ trailing 早触发）|
| P8 (dev=0.16) | -1.23 | -0.25 (死水救) | -0.25 |
| **合计** | **-0.99** | **+1.15** | **约 +1.5~+2.5** |

## 遗留问题

1. **观测期**：跑 2-3 天后看低 dev 区（0-0.15%）真实胜率/盈亏。如果连续亏损，回到 38 笔回测结论，把过滤加回。
2. **更细粒度替代方案**：未来可考虑把 0.15% 硬阈值改成参数化、或叠加盘口深度/成交量信号区分赢家与亏家，而不是单维度 dev 过滤。
3. **配置文件**：`config/config.json` 需要手动加 `fees` 块（与 `config.example.json` 对齐），否则用代码默认值（maker 0% / taker 7.2%，与默认一致，目前不影响）。
