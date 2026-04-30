#include "crypto/keccak256.h"

#include <cstring>
#include <stdexcept>

namespace polymarket {

namespace {

// Round constants for Keccak-f[1600], 24 rounds
constexpr uint64_t RC[24] = {
    0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808aULL,
    0x8000000080008000ULL, 0x000000000000808bULL, 0x0000000080000001ULL,
    0x8000000080008081ULL, 0x8000000000008009ULL, 0x000000000000008aULL,
    0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000aULL,
    0x000000008000808bULL, 0x800000000000008bULL, 0x8000000000008089ULL,
    0x8000000000008003ULL, 0x8000000000008002ULL, 0x8000000000000080ULL,
    0x000000000000800aULL, 0x800000008000000aULL, 0x8000000080008081ULL,
    0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL
};

// Rotation offsets, applied to lanes in PI order
constexpr int RHO[24] = {
    1,  3,  6,  10, 15, 21, 28, 36, 45, 55, 2,  14,
    27, 41, 56, 8,  25, 43, 62, 18, 39, 61, 20, 44
};

// Pi step lane permutation
constexpr int PI[24] = {
    10, 7,  11, 17, 18, 3,  5,  16, 8,  21, 24, 4,
    15, 23, 19, 13, 12, 2,  20, 14, 22, 9,  6,  1
};

constexpr int RATE_BYTES = 136;  // 1088 bits for Keccak-256

inline uint64_t rotl64(uint64_t x, int n) {
    return (x << n) | (x >> (64 - n));
}

void keccak_f1600(uint64_t state[25]) {
    for (int round = 0; round < 24; ++round) {
        // Theta
        uint64_t C[5];
        for (int i = 0; i < 5; ++i) {
            C[i] = state[i] ^ state[i + 5] ^ state[i + 10] ^ state[i + 15] ^ state[i + 20];
        }
        for (int i = 0; i < 5; ++i) {
            uint64_t D = C[(i + 4) % 5] ^ rotl64(C[(i + 1) % 5], 1);
            for (int j = 0; j < 25; j += 5) {
                state[j + i] ^= D;
            }
        }

        // Rho + Pi
        uint64_t t = state[1];
        for (int i = 0; i < 24; ++i) {
            int j = PI[i];
            uint64_t tmp = state[j];
            state[j] = rotl64(t, RHO[i]);
            t = tmp;
        }

        // Chi
        for (int j = 0; j < 25; j += 5) {
            uint64_t a[5];
            for (int i = 0; i < 5; ++i) a[i] = state[j + i];
            for (int i = 0; i < 5; ++i) {
                state[j + i] = a[i] ^ ((~a[(i + 1) % 5]) & a[(i + 2) % 5]);
            }
        }

        // Iota
        state[0] ^= RC[round];
    }
}

inline void absorb_block(uint64_t state[25], const uint8_t* block) {
    for (int i = 0; i < RATE_BYTES / 8; ++i) {
        uint64_t lane = 0;
        for (int b = 0; b < 8; ++b) {
            lane |= static_cast<uint64_t>(block[i * 8 + b]) << (b * 8);
        }
        state[i] ^= lane;
    }
}

}  // namespace

std::array<uint8_t, 32> keccak256(const uint8_t* data, size_t len) {
    uint64_t state[25] = {0};

    // Absorb full blocks
    while (len >= RATE_BYTES) {
        absorb_block(state, data);
        keccak_f1600(state);
        data += RATE_BYTES;
        len -= RATE_BYTES;
    }

    // Final block: copy + Keccak padding (0x01 ... 0x80)
    uint8_t block[RATE_BYTES] = {0};
    if (len > 0) std::memcpy(block, data, len);
    block[len] |= 0x01;            // Keccak domain (NOT 0x06 SHA3)
    block[RATE_BYTES - 1] |= 0x80;
    absorb_block(state, block);
    keccak_f1600(state);

    // Squeeze 32 bytes
    std::array<uint8_t, 32> out{};
    for (int i = 0; i < 32; ++i) {
        out[i] = static_cast<uint8_t>((state[i / 8] >> ((i % 8) * 8)) & 0xff);
    }
    return out;
}

std::string hex_encode(const uint8_t* bytes, size_t len) {
    static const char* H = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(H[(bytes[i] >> 4) & 0xf]);
        out.push_back(H[bytes[i] & 0xf]);
    }
    return out;
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::vector<uint8_t> hex_decode(const std::string& hex) {
    size_t start = 0;
    if (hex.size() >= 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) start = 2;
    if ((hex.size() - start) % 2 != 0) {
        throw std::invalid_argument("hex_decode: odd-length input");
    }
    std::vector<uint8_t> out;
    out.reserve((hex.size() - start) / 2);
    for (size_t i = start; i < hex.size(); i += 2) {
        int hi = hex_nibble(hex[i]);
        int lo = hex_nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) {
            throw std::invalid_argument("hex_decode: non-hex character");
        }
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return out;
}

}  // namespace polymarket
