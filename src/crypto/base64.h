#pragma once

// URL-safe Base64 (RFC 4648 §5) without padding.
// 字母表：A-Z a-z 0-9 - _   （把标准 base64 的 '+' '/' 换成 '-' '_'）
// 编码不带 '=' padding；解码同时容忍带与不带 padding。
//
// 用途：Polymarket CLOB L2 HMAC 签名（POLY_SIGNATURE header + api_secret 编码）。

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace polymarket {

std::string base64url_encode(const uint8_t* data, size_t len);
std::string base64url_encode(const std::vector<uint8_t>& data);

// 非法字符抛 std::invalid_argument
std::vector<uint8_t> base64url_decode(const std::string& s);

}  // namespace polymarket
