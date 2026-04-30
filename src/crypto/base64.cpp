#include "crypto/base64.h"

#include <stdexcept>

namespace polymarket {

static const char URL_ALPHABET[65] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static int decode_char(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

std::string base64url_encode(const uint8_t* data, size_t len, bool padded) {
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    size_t i = 0;
    while (i + 3 <= len) {
        uint32_t v = (uint32_t(data[i]) << 16) |
                     (uint32_t(data[i + 1]) << 8) |
                      uint32_t(data[i + 2]);
        out += URL_ALPHABET[(v >> 18) & 63];
        out += URL_ALPHABET[(v >> 12) & 63];
        out += URL_ALPHABET[(v >>  6) & 63];
        out += URL_ALPHABET[ v        & 63];
        i += 3;
    }
    size_t rem = len - i;
    if (rem == 1) {
        uint32_t v = uint32_t(data[i]) << 16;
        out += URL_ALPHABET[(v >> 18) & 63];
        out += URL_ALPHABET[(v >> 12) & 63];
        if (padded) out += "==";
    } else if (rem == 2) {
        uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8);
        out += URL_ALPHABET[(v >> 18) & 63];
        out += URL_ALPHABET[(v >> 12) & 63];
        out += URL_ALPHABET[(v >>  6) & 63];
        if (padded) out += "=";
    }
    return out;
}

std::string base64url_encode(const std::vector<uint8_t>& data, bool padded) {
    return base64url_encode(data.data(), data.size(), padded);
}

std::vector<uint8_t> base64url_decode(const std::string& s) {
    size_t in_len = s.size();
    while (in_len > 0 && s[in_len - 1] == '=') --in_len;  // 容忍 padding

    std::vector<uint8_t> out;
    out.reserve(in_len * 3 / 4);

    int buf  = 0;
    int bits = 0;
    for (size_t i = 0; i < in_len; ++i) {
        int v = decode_char(s[i]);
        if (v < 0)
            throw std::invalid_argument("base64url_decode: invalid char");
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((buf >> bits) & 0xff));
        }
    }
    return out;
}

}  // namespace polymarket
