#pragma once

// Keccak-256 (Ethereum 用的 hash，**不是** SHA3-256)
// 区别：padding domain byte = 0x01（SHA3 是 0x06），其余完全相同。
// 主要使用方：
//   - R2 wallet：keystore MAC + 派生 EVM 地址
//   - R4 EIP-712：domain separator、struct hash、typed data hash
// 实现参考：FIPS 202（KECCAK-p[1600, 24]）+ Keccak team 公有领域参考

#include <array>
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace polymarket {

// 单次哈希
std::array<uint8_t, 32> keccak256(const uint8_t* data, size_t len);

// 便捷重载
inline std::array<uint8_t, 32> keccak256(const std::string& s) {
    return keccak256(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

// 32-byte hash → 64 字符 hex（小写，无 "0x" 前缀）
std::string hex_encode(const uint8_t* bytes, size_t len);
inline std::string hex_encode(const std::array<uint8_t, 32>& h) {
    return hex_encode(h.data(), h.size());
}

// hex 字符串 → bytes（接受可选 "0x" 前缀；长度必须偶数）
// 失败抛 std::invalid_argument
std::vector<uint8_t> hex_decode(const std::string& hex);

}  // namespace polymarket
