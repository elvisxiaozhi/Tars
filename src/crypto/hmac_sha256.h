#pragma once

// HMAC-SHA256（OpenSSL 3.x EVP_MAC 后端）。
// 用途：Polymarket CLOB L2 鉴权签名。

#include <array>
#include <cstddef>
#include <cstdint>

namespace polymarket {

std::array<uint8_t, 32> hmac_sha256(const uint8_t* key, size_t key_len,
                                    const uint8_t* msg, size_t msg_len);

}  // namespace polymarket
