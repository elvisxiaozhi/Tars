#pragma once

#include <string>

// 构建/运行版本信息——用于把每条交易/候选标注到确定的代码+配置版本，
// 让后续分析能机械按 regime 切片（见 prompts/analyze_strategy.md 的 regime 切片规则），
// 不再依赖 git log + 时间戳手工重建。
//
// code_version：CMake 配置期注入的 git short sha（POLY_GIT_SHA）。
// config_hash ：启动加载 config 时由原始文件字节算出的稳定哈希，set 一次后只读。

#ifndef POLY_GIT_SHA
#define POLY_GIT_SHA "unknown"
#endif

namespace polymarket {

inline const std::string& code_version() {
    static const std::string v = POLY_GIT_SHA;
    return v;
}

inline std::string& config_hash_ref() {
    static std::string h = "unset";
    return h;
}

inline const std::string& config_hash() { return config_hash_ref(); }

inline void set_config_hash(const std::string& h) { config_hash_ref() = h; }

}  // namespace polymarket
