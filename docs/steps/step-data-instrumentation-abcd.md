# Step data-instrumentation — A+B+C+D 数据基建

## 概要

为让后续策略分析"可行且可靠"（而非仅方便），落地 4 项数据基建:A=历史样本启动快照防丢失;B=每条交易/候选标注 `code_version`+`config_hash` 以机械切 regime;C=主策略被拒候选落盘 `main_candidates.jsonl`;D=迁移/重置运维纪律文档化。**全程不改任何交易/实验策略逻辑**,纯数据旁路 + 字段追加。

## 关键命令

```bash
cmake -S . -B build && cmake --build build -j   # 配置期会打印 POLY_GIT_SHA
./build/polymarket-arb ./config/config.json      # 生效后:trades.jsonl 含新字段,logs/backup/ 出快照
```

## 文件清单

| 操作 | 文件 |
|------|------|
| 新增 | `src/core/build_info.h`（code_version/config_hash 访问器） |
| 修改 | `CMakeLists.txt`（execute_process 注入 POLY_GIT_SHA 编译期宏） |
| 修改 | `src/utils/config.cpp`（load_config 读原始字节算 config_hash → set_config_hash） |
| 修改 | `src/core/trade_journal.cpp`（A:启动快照到 logs/backup/ 留 10 份;B:record() 加 2 字段） |
| 修改 | `src/core/experiment_engine.cpp`（B:append_jsonl() 加 2 字段） |
| 修改 | `src/main.cpp`（C:log_main_candidate() + reject 分支调用） |
| 修改 | `docs/RUNBOOK.md`（§5.5 A/D 纪律 + B/C 字段说明） |
| 新增 | `docs/steps/step-data-instrumentation-abcd.md` |

## 设计决策

- **A 不做 deploy 脚本改造**：根因是运营迁移到 159.203.56.165 时旧历史没带过来，bot 代码本身 load+append 正确（trade_journal.cpp 已 `std::ios::app`）。代码侧能做的最高价值是"启动快照"——让任何迁移/误删可恢复;迁移纪律归 D 文档。
- **B 用编译期宏 + 运行期 hash**：`code_version` 走 CMake `execute_process` 注入（configure 期求值，已知局限:改代码需重跑 cmake 刷新——已在 build_info.h/RUNBOOK 注明）;`config_hash` 用 `std::hash` 原始文件字节,够稳定标识"哪份配置",不引入额外依赖。
- **B 不加 TradeRecord 结构字段**：值是进程内常量,分析只读 jsonl;只在 2 个序列化点 + C 写出,避免在几十处 TradeRecord 填充点改代码（回归面最小）。两个序列化器（trade_journal `record()` / experiment `append_jsonl()`）独立,各加一次。
- **B 同时写实验 jsonl**：用户未限定仅主策略;实验 jsonl 多两个只读字段无行为影响,反而利于实验自身按 regime 切片。
- **C 只挂在 `sig.reject_reason` 分支（main.cpp:1680）**：pre-sig 的粗粒度 reject（time_too_short_loop/coin_position_open 等经 reject_agg）是循环/仓位状态,不是"gate 拒真候选";真正用于"gate 是否过紧"分析的恰是 strategy 级 `sig.reject_reason`。catch(...) 全包,诊断旁路永不影响主循环。
- **C 不节流**：用户要完整性做 gate 分析,完整性 > 磁盘;类比实验 `*_candidates.jsonl`（数 MB/天）可接受,RUNBOOK 注明定期 archive。

## 红线遵守

- [x] 没有引入硬编码密钥
- [x] 没有跳过错误处理（快照/候选均 try-catch,失败仅 warn,不影响主循环/交易）
- [x] 编译通过,无 warning（改动文件 grep warning/error 为空;`POLY_GIT_SHA = 6f54fcc` 已注入,strings 验证）
- [x] 未改任何 evaluate_*/exit/TP/risk 逻辑——交易行为零变更

## 验收结果

```
cmake: -- POLY_GIT_SHA = 6f54fcc
build: [100%] Built target polymarket-arb   EXIT=0
改动文件无 warning/error
strings build/polymarket-arb | grep '^6f54fcc$' → 命中（code_version 注入 OK）
```

## 遗留问题

- **未做端到端运行验证**：本地无代理/行情,没实跑产出带新字段的 jsonl / 触发快照 / 写 main_candidates.jsonl。需 review 后在服务器 `cmake -S . -B build && cmake --build build -j` 重编 + 由 manager 重启 bot,再核对:
  1. `logs/backup/trades.<ms>.jsonl` 出现;
  2. 新 trade 行含 `code_version`/`config_hash`;
  3. `logs/main_candidates.jsonl` 开始增长且字段齐全。
- 重启服务器 bot 会丢会话内连胜连亏状态——需用户确认时机后再部署。
- `code_version` 改代码后须重跑 `cmake -S . -B build` 才刷新（configure 期求值的固有局限,已文档化;可接受,因 regime 主要由 config 驱动,`config_hash` 是运行期实时的）。
