# Step — 退役 eth_late_cheap_v1（含证据）

**日期**：2026-05-23
**结论**：从 main.cpp 摘除实例,停止运行。策略逻辑(`evaluate_eth_late_cheap_*`)按惯例保留在 experiment_engine.cpp 作回放参考,**不要重新实例化**。

---

## 这个策略是什么

ETH-only 末段逆势 cheap-value:mr 10-25 入场、买较便宜侧(0.20-0.28¢)、四分之一仓($0.25)、紧 TP(entry+8¢卖50% / entry+16¢卖50%)。变量名历史遗留叫 `trend_experiment`,id 前缀 `L`。

## 为什么退役 —— 数据(2026-05-16 ~ 05-23,46 笔去重)

**总 PnL −$0.475,胜率 28%(13/46),每笔均值 −$0.01。**

| exit_reason | 笔数 | PnL |
|---|---|---|
| eth_late_stop_price | 13 | −1.11 |
| eth_late_no_start_exit | 8 | −0.37 |
| expired | 15 | +0.17 |
| eth_late_trailing_stop | 10 | +0.83 |

### 三条独立证据

1. **被同门严格压制**。同样 ETH、同样 cheap-value 家族:
   - `eth_cheap_v1`(宽 TP, mr>30):104 笔,**+$0.078,41% 胜率**
   - `eth_late_cheap_v1`(紧 TP, mr 21-25):46 笔,**−$0.475,28% 胜率**
   两者并行同周,late 变体在每个维度都更差。

2. **盈利全靠 3 笔运气**。max 涨幅 >+0.30 的肥尾仅 3 笔却贡献 +$2.50;**剔掉这 3 笔,剩 43 笔为 −$2.98**。93% 的交易在净亏,只靠 6% 的离群单避免灾难——非稳健结构。

3. **入场质量是死结,且无杠杆可改**:
   - 29/46 笔(63%)的 max 连 entry+0.08 都没到——便宜侧压根不反弹。
   - 测过把紧 TP 换成宽 TP(0.42/0.62):**−$0.475 → −$2.60**,更差。紧 TP 是适配短窗口的正确设计,不是可改的杠杆(短窗口里价格没时间涨到 0.42)。
   - "末段窗口"特征根本不成立:46 笔**全部**落在 mr 21-25,允许的 10-20 段从不入场。所谓"late"差异化不存在。

## 置信度声明(诚实标注)

**不能**统计上断言它"必然亏":46 笔 / 1 周,28% 胜率的 95% 区间约 [15%, 41%],break-even 需要的 ~31% 落在区间内。

退役依据不是"证明它亏",而是:**(a) 找不到任何 edge,(b) 被 eth_cheap_v1 在同赛道用更优结构压制,(c) 无可识别的优化杠杆,(d) 盈利依赖 3 笔离群**。继续跑只是在薄数据上消耗注意力,没有反超 eth_cheap 的机制理由。

## ⚠️ 给未来:不要重蹈的模式

把以下"逆势 cheap-value 变体"重新做成新策略前,先回看本文档——它们大概率重复同样的失败:

- **"在更晚的窗口(mr<25)做 cheap-value"**:窗口越短,均值回归越没时间发生,胜率必然低于宽窗版。已验证 late(28%) < regular(41%)。
- **"用紧 TP 锁小利的 cheap-value"**:cheap-value 的正期望全靠少数 0.25→0.7+ 的肥尾;紧 TP 在短窗口虽是对的,但它配的低胜率入场救不回来。要赚 cheap-value 的钱,得用宽窗 + 宽 TP(即 eth_cheap_v1 路线),不是反过来。
- **判据**:任何新 cheap-value 变体上线前,必须先证明它在影子样本里**胜率 > eth_cheap_v1 的 ~41%** 或**每笔均值 > eth_cheap_v1**,否则就是本策略的换皮,直接否决。

## 文件改动

| 操作 | 文件 |
|------|------|
| 删实例 + 6 处引用 | `src/main.cpp` |
| 删 3 个路由声明 | `src/net/api_server.h` |
| 删 callback成员 + setter + 路由 | `src/net/api_server.cpp` |
| 删 "Trend Follow" tab + 面板 + JS 引用 | `src/dashboard.h` |
| 保留(惯例) | `src/core/experiment_engine.cpp` 的 `evaluate_eth_late_cheap_*`(休眠,作回放) |

## 验收

```
cmake --build build -j   → 100% Built
test_crypto / test_eip712 → PASS
dashboard Promise.all 对齐校验:17 vars = 17 fetchJSON ✓
```
