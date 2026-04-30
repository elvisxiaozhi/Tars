#include "crypto/eip712.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include <secp256k1_recovery.h>

#include "crypto/keccak256.h"
#include "crypto/wallet.h"

namespace polymarket {

// ── Signature ─────────────────────────────────────────────────────────────────

std::string Signature::to_hex() const {
    std::string out = "0x";
    out += hex_encode(r, 32);
    out += hex_encode(s, 32);
    char buf[3];
    std::snprintf(buf, sizeof(buf), "%02x", v);
    out += buf;
    return out;
}

// ── Atomic encoding ───────────────────────────────────────────────────────────

std::array<uint8_t, 32> eip712_uint256(uint64_t v) {
    std::array<uint8_t, 32> out = {};
    for (int i = 7; i >= 0; --i) {
        out[24 + i] = v & 0xff;
        v >>= 8;
    }
    return out;
}

std::array<uint8_t, 32> eip712_uint256_str(const std::string& decimal) {
    if (decimal.empty())
        throw std::invalid_argument("eip712_uint256_str: empty string");

    std::array<uint8_t, 32> out = {};
    for (char c : decimal) {
        if (c < '0' || c > '9')
            throw std::invalid_argument("eip712_uint256_str: non-decimal char");
        uint16_t carry = static_cast<uint16_t>(c - '0');
        // Multiply big-endian byte array by 10, add carry (LSB-first)
        for (int i = 31; i >= 0; --i) {
            uint16_t cur = static_cast<uint16_t>(out[i]) * 10 + carry;
            out[i]       = cur & 0xff;
            carry        = cur >> 8;
        }
        if (carry)
            throw std::overflow_error("eip712_uint256_str: value overflows uint256");
    }
    return out;
}

std::array<uint8_t, 32> eip712_address(const std::string& hex_addr) {
    std::string hex = hex_addr;
    if (hex.size() >= 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X'))
        hex = hex.substr(2);
    if (hex.size() != 40)
        throw std::invalid_argument(
            "eip712_address: expected 40 hex chars, got " + std::to_string(hex.size()));

    auto raw = hex_decode(hex);  // 20 bytes
    std::array<uint8_t, 32> out = {};
    std::copy(raw.begin(), raw.end(), out.begin() + 12);
    return out;
}

std::array<uint8_t, 32> eip712_bool(bool v) {
    std::array<uint8_t, 32> out = {};
    out[31] = v ? 1 : 0;
    return out;
}

std::array<uint8_t, 32> eip712_bytes32(const std::array<uint8_t, 32>& v) {
    return v;
}

std::array<uint8_t, 32> eip712_string(const std::string& s) {
    return keccak256(s);
}

std::array<uint8_t, 32> eip712_bytes(const std::vector<uint8_t>& v) {
    return keccak256(v.data(), v.size());
}

// ── Type and struct hashing ───────────────────────────────────────────────────

std::array<uint8_t, 32> eip712_type_hash(const std::string& full_type_string) {
    return keccak256(full_type_string);
}

std::array<uint8_t, 32> eip712_struct_hash(
    const std::array<uint8_t, 32>& th,
    const std::vector<std::array<uint8_t, 32>>& fields)
{
    std::vector<uint8_t> buf((1 + fields.size()) * 32);
    std::copy(th.begin(), th.end(), buf.begin());
    for (size_t i = 0; i < fields.size(); ++i)
        std::copy(fields[i].begin(), fields[i].end(), buf.begin() + (1 + i) * 32);
    return keccak256(buf.data(), buf.size());
}

// ── Domain separator ──────────────────────────────────────────────────────────

std::array<uint8_t, 32> eip712_domain_separator(const EIP712Domain& d) {
    bool has_contract = !d.verifying_contract.empty();

    std::string type_str = "EIP712Domain(string name,string version,uint256 chainId";
    if (has_contract) type_str += ",address verifyingContract";
    type_str += ")";

    auto th = eip712_type_hash(type_str);

    std::vector<std::array<uint8_t, 32>> fields;
    fields.push_back(eip712_string(d.name));
    fields.push_back(eip712_string(d.version));
    fields.push_back(eip712_uint256(d.chain_id));
    if (has_contract)
        fields.push_back(eip712_address(d.verifying_contract));

    return eip712_struct_hash(th, fields);
}

// ── Final digest ──────────────────────────────────────────────────────────────

std::array<uint8_t, 32> eip712_encode(
    const std::array<uint8_t, 32>& domain_sep,
    const std::array<uint8_t, 32>& struct_hash)
{
    uint8_t buf[66];
    buf[0] = 0x19;
    buf[1] = 0x01;
    std::copy(domain_sep.begin(), domain_sep.end(), buf + 2);
    std::copy(struct_hash.begin(), struct_hash.end(), buf + 34);
    return keccak256(buf, 66);
}

// ── ECDSA signing ─────────────────────────────────────────────────────────────

static secp256k1_context* signing_ctx() {
    // libsecp256k1 sign_recoverable requires SIGN context.
    static secp256k1_context* ctx =
        secp256k1_context_create(SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);
    return ctx;
}

Signature eip712_sign(const PrivateKey& key, const std::array<uint8_t, 32>& digest) {
    secp256k1_ecdsa_recoverable_signature raw;
    if (!secp256k1_ecdsa_sign_recoverable(
            signing_ctx(), &raw, digest.data(), key.data(), nullptr, nullptr)) {
        throw std::runtime_error("eip712_sign: secp256k1_ecdsa_sign_recoverable failed");
    }

    uint8_t compact[64];
    int recid = 0;
    secp256k1_ecdsa_recoverable_signature_serialize_compact(signing_ctx(), compact, &recid, &raw);

    // libsecp256k1 guarantees low-s by default (s ≤ n/2).
    Signature sig;
    std::copy(compact,      compact + 32, sig.r);
    std::copy(compact + 32, compact + 64, sig.s);
    sig.v = static_cast<uint8_t>(27 + recid);
    return sig;
}

}  // namespace polymarket
