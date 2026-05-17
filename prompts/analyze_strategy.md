# 单策略深度分析 Prompt (v2)

> 用法：每次分析换 `{{STRATEGY_FILE}}` 占位符（例如 `logs/trades.jsonl` 或 `logs/experiment_trend_trades.jsonl`），整段贴给 AI。

---

# 任务：深度分析单个策略

## 目标（按权重，不能并列）
1. 把净 PnL（已含 fee）从负转正 ← 唯一首要目标
2. 在保住净 PnL≥0 的前提下 *维持* 现有交易频次（19-23 笔/天已是中高频，不再追求提高）
3. 拒绝任何为提高表面胜率而压低 R/R 的方案

## 数据约束（违反 = 结论作废）

### 字段语义
- `pnl` 字段已是净值（gross − fee）。**严禁** 二次减 fee
- `gross_pnl = pnl + fee`，`fee_ratio = fee / abs(pnl + fee)`
- 当样本 n < 50 时，禁止给出胜率/夏普，只能写 "样本不足"

### 时间窗切片（必须做）
对 {{STRATEGY_FILE}}：先按 git log 切片，禁止跨 regime 合并。已知 regime 边界：
- 2026-04-21：P&L double-counting 修复 → 数据起点（更早的数据不可信）
- 2026-05-04 ~ 05-05：unlimited dry-run 实验日，*必须单独分桶* 或剔除
- 2026-05-06：恢复单仓制
- 2026-05-09：trend_follow confidence scoring 加入
- 2026-05-10：tighten quiet_rebound + confidence gates
- 2026-05-14：ETH experiments 重调
分析时输出每个 regime 段的 n / 净 PnL / fee_ratio，禁止给"全期"汇总数字

### exit_reason 解读
- `tp0/tp1/tp2`：胜率 100% 是恒等式（触发条件即正盈）。**只能** 用作 "频次 / 单笔规模" 维度，不能用作 alpha 证据
- `stop_price`：策略真正的损失源，重点分析
- `trailing_stop`：真正的 alpha 信号，胜率有意义
- `dead_water_exit / fast_fail_exit / cheap_fail_stop`：保护性退出，要看是不是"过度保护"
- `expired`：超时退出，需要单独看 minutes_remaining 分布

### 字段黑名单（填充率 < 50% 不要用）
以下字段如果稀疏（< 50% 行有值），禁止作为分析支柱：
- confidence_components, cross_coin_state, entry_confidence
- btc_alignment, eth_alignment
（请先 grep 验证填充率再用）

### 实时日志使用
若需查 bot.log，*必须* grep：
```
grep -E "ENTER|EXIT|SKIP|REJECT|SIGNAL|stop|tp[0-9]" logs/bot.log | tail -300
```
直接 tail 是行情广播，无信息。

## 强制分析步骤
1. 报告时间跨度、笔数、每天笔数密度
2. 按 regime 段切片汇总（n / gross_PnL / fee / fee_ratio / 净 PnL）
3. exit_reason 分桶（n / 单笔均盈 / 总贡献），按 |总贡献| 降序
4. 找最大亏损贡献的 exit_reason，钻取这部分子样本：
   - 按 coin / minutes_remaining 分位 / side / entry_price 分位 切片
   - 找出 "如果加一个过滤条件能去掉哪一坨亏损" 的可执行规则
5. fee_ratio 分析：是否手续费就吃掉了 alpha
6. mfe_capture_rate 分布（如有）：是否"吃到行情但没拿住"

## 输出格式
- 全用表格，禁止散文段落
- 每条参数建议必须包含：
  - 当前值 → 建议值
  - 在哪一段历史数据上回放
  - 预期净 PnL 变化（带正负号和金额）
  - 风险（"会去掉多少笔本来盈利的交易"）
- 最多 3 条建议，按 "预期净 PnL 改善 / 改动复杂度" 排序
- 末尾问我 1 个最需要补充的代码/配置上下文（不要泛问）

## 禁止
- 5-7 分打分（噪声太大）
- 策略间相关性（数据不支持）
- "保守/中性/激进" 多版本（直接给最优 + 风险）
- 建议监控"orderbook 时序、信号延迟、成交速度"（日志没这些字段）
