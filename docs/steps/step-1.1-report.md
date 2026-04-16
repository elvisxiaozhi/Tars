# Step 1.1 — CMake 项目搭建 + 依赖集成

## 概要

搭建 CMake 构建系统，集成全部 6 个外部依赖，验证编译链接和运行。

## 关键命令

```bash
# brew 安装
brew install boost cmake secp256k1

# 手动依赖
curl -sL https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp -o third_party/json.hpp
git clone --depth 1 --branch v1.15.2 https://github.com/gabime/spdlog.git third_party/spdlog

# 编译运行
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/polymarket-arb
```

## 文件清单

| 操作 | 文件 |
|------|------|
| 新增 | `CMakeLists.txt` |
| 新增 | `src/main.cpp` |
| 新增 | `third_party/json.hpp` (nlohmann/json 3.11.3) |
| 新增 | `third_party/spdlog/` (spdlog 1.15.2, git clone) |
| 新增 | 目录结构 `src/{core,net,crypto,storage,cli,utils}`, `config/`, `tests/` |

## 设计决策

- **Boost header-only 模式**：Boost 1.90 的 Beast (HTTP/WS) 是 header-only，不需要编译 Boost 库，`find_package(Boost)` 不指定 COMPONENTS
- **spdlog 编译为静态库**：通过 `add_subdirectory` 集成，避免 header-only 模式增加编译时间
- **nlohmann/json 单文件**：直接下载 `json.hpp`，最简方式
- **secp256k1 用 brew**：brew 提供的是 bitcoin-core 维护的 libsecp256k1，质量可靠

## 红线遵守

- [x] 没有引入硬编码密钥
- [x] 没有跳过错误处理
- [x] 编译通过，零 warning

## 验收结果

```
[2026-04-17 01:00:37.698] [info] polymarket-arb v0.1.0
[2026-04-17 01:00:37.698] [info] boost     1.90.0
[2026-04-17 01:00:37.698] [info] openssl   OpenSSL 3.6.1 27 Jan 2026
[2026-04-17 01:00:37.698] [info] sqlite    3.51.0
[2026-04-17 01:00:37.698] [info] nlohmann  3.11.3
[2026-04-17 01:00:37.698] [info] secp256k1 OK
[2026-04-17 01:00:37.698] [info] all dependencies verified
```

## 遗留问题

- `third_party/spdlog/` 是 git clone 的完整仓库（含 .git），提交时需要决定是作为 submodule 还是删掉 `.git` 直接纳入
