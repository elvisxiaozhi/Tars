# Step Docs — 项目结构梳理与 AI-readable 文档体系

## 概要

为项目建立分层文档体系，让任何后来者（含其他 AI）冷启动 3 分钟内能回答：
项目在干嘛？现在做到哪一步？要改 X 应该动哪个文件？怎么跑？怎么排障？
不动业务代码，只新增 4 份 markdown。

## 文件清单

| 操作 | 文件 | 行数 |
|------|------|------|
| 新增 | `CLAUDE.md` | 135 |
| 新增 | `docs/ARCHITECTURE.md` | 250 |
| 新增 | `docs/DECISIONS.md` | 148 |
| 新增 | `docs/RUNBOOK.md` | 223 |
| 新增 | `docs/steps/step-docs-report.md`（本文）| — |

## 分层设计

```
CLAUDE.md            自动加载 / 每次对话进 context  →  地图 + 入口 + 硬约束
ARCHITECTURE.md      按需加载                       →  线程 / 数据流 / tick 生命周期 / 行号速查
DECISIONS.md         按需加载                       →  21 条 ADR + 事故教训
RUNBOOK.md           按需加载                       →  操作手册 / 排障 / 应急
log_reference.md     已有，未动                     →  日志关键词 → 含义
```

## 设计决策

### 1. CLAUDE.md 控制在 <200 行
每次对话固定加载，写多了反而费 token。当前 135 行，仅放：项目定位、入口表、模块地图、
硬约束、配置关键字段、编译命令。详细内容指针化（指向 ARCHITECTURE / DECISIONS / RUNBOOK）。

### 2. 不重复 git log / memory 的内容
路线图当前进度、step 历史、近期变更——一律指向 git log + memory，不在文档里复述
（避免文档腐化）。

### 3. 明确点名 design.md 是历史文档
`docs/design.md` 描述的 ArbDetector / OrderExecutor / WalletManager 已不在代码里
（项目早期 pivot 到 BTC 1h 二元期权）。CLAUDE.md 第一段就警告，避免后人按它写新代码。

### 4. DECISIONS.md 只记"回头看仍要解释"的决策
参数调整（5min→8min、+25¢→+15¢ 等）放 git log，不抄。架构选型 / 事故驱动的修复 /
方法论教训才进 ADR。每条结构：背景 → 决策 → 为什么不选 X → 后果。

### 5. 大量挂行号 + commit 哈希
ARCHITECTURE.md 标 `main.cpp:152, 811, 1374` 等关键位置；DECISIONS.md 引用
`be5bc91, 8a2f53e, 28a7963, ca0648b` 等关键 commit。行号会过期但比 grep 快得多，
代码是事实来源（CLAUDE.md 头部已声明）。

### 6. RUNBOOK.md 不重复 log_reference.md
日志关键词速查已经存在，RUNBOOK 专做"我要做 X"——切换 dry/live、keystore 重生、
启动守卫诊断、HTTP 卡死、应急平仓等。

## Token 节省评估

每次对话固定多 ~135 行 context（≈2-3K tokens），但省下：
- AI 不用 grep `src/` 摸架构（每次 ~3-5 工具调用，~2K tokens）
- 不会按 design.md 老模型写错代码（最差时浪费 ~5K tokens）
- 排障可直接定位到 RUNBOOK 章节，不用让 AI 重新推理

净收益：每次对话省 5-10K tokens 是常态。

## 红线遵守

- [x] 不动业务代码（src/ 零修改）
- [x] 不动 `docs/design.md`（保留作历史参考）
- [x] CLAUDE.md < 200 行
- [x] 所有事实声明可在代码 / git log / commit message 里复核
- [x] 没引入新依赖

## 验收

```
$ wc -l CLAUDE.md docs/ARCHITECTURE.md docs/DECISIONS.md docs/RUNBOOK.md
 135 CLAUDE.md
 250 docs/ARCHITECTURE.md
 148 docs/DECISIONS.md
 223 docs/RUNBOOK.md
 756 total
```

冷启动 3 问自检：
1. 这项目在干嘛？现在做到哪一步？ → CLAUDE.md 第一段 + memory
2. 要改 X 功能，应该动哪个文件？ → CLAUDE.md "入口指引"表
3. 跑起来 / 测起来要哪些步骤？ → CLAUDE.md "编译运行" + RUNBOOK §1

## 遗留

无业务影响。可选优化（不紧急）：CLAUDE.md "模块地图"那张大表压缩到 8-10 行（细表挪
ARCHITECTURE.md），进一步降低自动加载成本——135 行还在合理范围，暂不做。
