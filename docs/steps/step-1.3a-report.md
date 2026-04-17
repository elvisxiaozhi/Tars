# Step 1.3a — Xcode 项目生成配置

## 概要

配置 CMake 生成 Xcode 项目，方便手动编译、调试和代码浏览。

## 关键命令

```bash
# 方式一：一键脚本
./scripts/xcode-gen.sh

# 方式二：手动
cmake -B build-xcode -G Xcode
open build-xcode/polymarket-arb.xcodeproj
```

Xcode 打开后：
- 左上角选 scheme `polymarket-arb`（不是 ALL_BUILD）
- 选 Release 或 Debug
- `Cmd+B` 编译，`Cmd+R` 运行

## 文件清单

| 操作 | 文件 |
|------|------|
| 新增 | `scripts/xcode-gen.sh` — 一键生成并打开 Xcode 项目 |
| 修改 | `.gitignore` — 增加 `build-xcode/` |

## 设计决策

- **CMake -G Xcode**：不另外维护 .xcodeproj，从 CMakeLists.txt 单一来源生成，改了 CMake 重新跑脚本即可
- **build-xcode/ 与 build/ 分开**：Xcode 生成物不影响 CLI 的 build 目录，两种方式可以共存
- **脚本放 scripts/**：统一管理辅助脚本

## 红线遵守

- [x] 没有引入硬编码密钥
- [x] Xcode 编译通过，运行正常
- [x] build-xcode/ 已加入 .gitignore

## 验收结果

```
cmake -B build-xcode -G Xcode          → Configuring done
cmake --build build-xcode --config Release → ** BUILD SUCCEEDED **
./build-xcode/Release/polymarket-arb    → 正常运行，输出与 CLI build 一致
```

## 遗留问题

- CMakeLists.txt 变更后需重新运行 `scripts/xcode-gen.sh` 刷新 Xcode 项目
