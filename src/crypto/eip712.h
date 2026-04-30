#pragma once

// EIP-712 typed structured data signing (Ethereum)
//
// Low-level usage:
//   1. Encode fields: eip712_uint256 / eip712_address / eip712_string / …
//   2. structHash = eip712_struct_hash(type_hash, fields)
//   3. domainSep  = eip712_domain_separator(domain)
//   4. digest     = eip712_encode(domainSep, structHash)
//   5. sig        = eip712_sign(key, digest)
//
// For Polymarket orders and CLOB auth, use the helpers in core/polymarket_types.h.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace polymarket {

class PrivateKey;

// 65-byte ECDSA signature: r[32] || s[32] || v[1]  (v = 27 or 28)
struct Signature {
    uint8_t r[32] = {};
    uint8_t s[32] = {};
    uint8_t v     = 0;

    // "0x" + hex(r) + hex(s) + hex2(v)  — 132 chars
    std::string to_hex() const;
};

// ── Atomic encoding: each returns one 32-byte ABI word ───────────────────────

std::array<uint8_t, 32> eip712_uint256(uint64_t v);
// Arbitrary-precision decimal string → big-endian 32 bytes (for large tokenIds)
std::array<uint8_t, 32> eip712_uint256_str(const std::string& decimal);
// "0x…" EVM address → 12-byte left-pad + 20-byte address
std::array<uint8_t, 32> eip712_address(const std::string& hex_addr);
std::array<uint8_t, 32> eip712_bool(bool v);
// bytes32 passthrough
std::array<uint8_t, 32> eip712_bytes32(const std::array<uint8_t, 32>& v);
// string → keccak256(utf8_bytes)
std::array<uint8_t, 32> eip712_string(const std::string& s);
// bytes  → keccak256(bytes)
std::array<uint8_t, 32> eip712_bytes(const std::vector<uint8_t>& v);

// ── Type and struct hashing ───────────────────────────────────────────────────

// typeHash = keccak256(full_type_string)
// Primary type with referenced deps: "Mail(Person from,…)Person(string name,…)"
std::array<uint8_t, 32> eip712_type_hash(const std::string& full_type_string);

// structHash = keccak256(typeHash || field[0] || … || field[n-1])
// Nested-struct fields should be their own structHash (already 32 bytes).
std::array<uint8_t, 32> eip712_struct_hash(
    const std::array<uint8_t, 32>& th,
    const std::vector<std::array<uint8_t, 32>>& fields);

// ── Domain separator ──────────────────────────────────────────────────────────

struct EIP712Domain {
    std::string name;
    std::string version;
    uint64_t    chain_id          = 0;
    std::string verifying_contract;  // "0x…" — leave empty to omit from type
};

std::array<uint8_t, 32> eip712_domain_separator(const EIP712Domain& d);

// ── Final digest ──────────────────────────────────────────────────────────────

// keccak256("\x19\x01" || domainSep || structHash)
std::array<uint8_t, 32> eip712_encode(
    const std::array<uint8_t, 32>& domain_sep,
    const std::array<uint8_t, 32>& struct_hash);

// ── ECDSA signing ─────────────────────────────────────────────────────────────

// Sign 32-byte digest with secp256k1.
// Guarantees low-s (libsecp256k1 default); v = recovery_id + 27.
Signature eip712_sign(const PrivateKey& key, const std::array<uint8_t, 32>& digest);

}  // namespace polymarket
