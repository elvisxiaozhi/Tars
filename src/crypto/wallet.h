#pragma once

// Web3 keystore v3 加密/解密 + EVM 地址派生
//
// keystore v3 格式参考：
//   https://github.com/ethereum/wiki/wiki/Web3-Secret-Storage-Definition
// 我们用 scrypt(N=2^18, r=8, p=1) + AES-128-CTR + Keccak-256(MAC)，与 geth 默认一致。
//
// 私钥处理原则：
//   - 函数签名用裸指针/数组，不持有所有权
//   - 调用者负责调用 secure_zero() 清擦
//   - 永远不输出私钥到日志（只输出地址）

#include <cstddef>
#include <cstdint>
#include <string>

namespace polymarket {

// 32-byte 私钥包装（栈上、自动析构清擦）
class PrivateKey {
public:
    PrivateKey() = default;
    explicit PrivateKey(const uint8_t bytes[32]);
    PrivateKey(const PrivateKey&) = delete;
    PrivateKey& operator=(const PrivateKey&) = delete;
    PrivateKey(PrivateKey&& other) noexcept;
    PrivateKey& operator=(PrivateKey&& other) noexcept;
    ~PrivateKey();

    const uint8_t* data() const { return bytes_; }
    bool valid() const { return valid_; }

    // 从 hex 字符串构造（接受 "0x..." 或裸 hex；长度必须 == 64 hex chars）
    static PrivateKey from_hex(const std::string& hex);

private:
    uint8_t bytes_[32] = {0};
    bool valid_ = false;
};

// privkey × G → 公钥 → keccak256 → 取后 20 字节 → EIP-55 mixed case
// 返回 "0xAbc..." 格式（42 字符）
std::string derive_address(const PrivateKey& key);

// 加密成 Web3 keystore v3 JSON 字符串
// password 长度 ≥ 1（推荐 ≥ 12）；空密码会抛
std::string encrypt_keystore(const PrivateKey& key, const std::string& password);

// 解密 keystore v3 JSON 字符串
// 密码错误抛 std::runtime_error("invalid password (MAC mismatch)")
PrivateKey decrypt_keystore(const std::string& json_str, const std::string& password);

// 安全清擦内存（避免编译器优化掉 memset）
void secure_zero(void* ptr, size_t len);

// 从 stdin 读密码（关回显），prompt 写到 stderr 不含换行
// 调用者用完应尽快 secure_zero(s.data(), s.size())
std::string read_password(const std::string& prompt);

}  // namespace polymarket
