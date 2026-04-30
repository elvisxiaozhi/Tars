#pragma once

// Polymarket-specific EIP-712 structs and signing helpers.
// Generic EIP-712 primitives live in crypto/eip712.h.

#include <array>
#include <cstdint>
#include <string>

namespace polymarket {

class PrivateKey;
struct Signature;

// ── CTFExchange V2 Order ──────────────────────────────────────────────────────
// V2（2026-04-28 升级生效）字段对齐 ctf-exchange-v2/src/exchange/libraries/Structs.sol。
// 与 V1 比：删除 taker / expiration / nonce / feeRateBps；新增 signer / timestamp / metadata / builder。

struct PolyOrder {
    uint64_t    salt;           // random nonce
    std::string maker;          // 持仓方（proxy address，收 USDC 持 CTF）
    std::string signer;         // 签名方（EOA "0x…"），signature_type=POLY_PROXY 时与 maker 不同
    std::string token_id;       // CTF outcome token ID — large uint256 as decimal string
    uint64_t    maker_amount;   // pUSD micro-units (6 decimals); 5_000_000 = $5.00
    uint64_t    taker_amount;   // shares micro-units
    uint8_t     side;           // 0 = BUY, 1 = SELL
    uint8_t     signature_type; // 0 = EOA, 1 = POLY_PROXY, 2 = POLY_GNOSIS_SAFE, 3 = POLY_1271
    uint64_t    timestamp;      // 毫秒（V2 新增；防订单回放）
    std::array<uint8_t, 32> metadata = {};  // V2 新增，bytes32（保留字段，通常 0）
    std::array<uint8_t, 32> builder  = {};  // V2 新增，bytes32（builderCode；自交易填 0）
};

// ── CLOB API key authentication ───────────────────────────────────────────────

struct ClobAuthData {
    std::string address;    // EOA "0x…"
    uint64_t    timestamp;  // unix seconds
    uint64_t    nonce;      // 0 for key creation
    std::string message;    // consent text (use CLOB_AUTH_MESSAGE below)
};

// 必须与 py-clob-client/signing/eip712.py 的 MSG_TO_SIGN 完全一致
static const char* CLOB_AUTH_MESSAGE =
    "This message attests that I control the given wallet";

// ── L2 HMAC API credentials ───────────────────────────────────────────────────
// R5 拿到的三件套；read balance / place order 等 L2 端点都用它们。

struct ApiCreds {
    std::string api_key;         // GUID 风格
    std::string api_secret;      // url-safe base64 编码的 HMAC key
    std::string api_passphrase;  // 任意字符串
};

// ── Signing helpers ───────────────────────────────────────────────────────────

// Sign a Polymarket limit order on CTFExchange V2 (Polygon mainnet, chain 137).
Signature sign_poly_order(const PrivateKey& key, const PolyOrder& order,
                          uint64_t chain_id = 137);

// Sign CLOB API authentication on ClobAuthDomain.
Signature sign_clob_auth(const PrivateKey& key, const ClobAuthData& auth,
                         uint64_t chain_id = 137);

// Build POLY_SIGNATURE for L2 HMAC auth.
// message = timestamp + method + request_path [+ body 单引号→双引号标准化]
// secret 是 url-safe base64 字符串（HMAC key 的编码形式）
// 返回 url-safe base64 的 HMAC-SHA256 签名（无 padding）
std::string build_hmac_l2(const std::string& api_secret_b64,
                          const std::string& timestamp,
                          const std::string& method,
                          const std::string& request_path,
                          const std::string& body = "");

// Serialize PolyOrder + signature → V2 POST /order compact JSON。
// 字段顺序 / 类型与 py-clob-client-v2 dry-sign 输出严格一致（KAT 验证）：
//   salt, maker, signer, tokenId, makerAmount, takerAmount,
//   side, signatureType, timestamp, metadata, builder, expiration, signature
// expiration 是 API body 字段（不参与 EIP-712 hash），默认 "0"（无过期）。
std::string polyorder_to_json(const PolyOrder& order, const Signature& sig,
                              const std::string& expiration_str = "0");

}  // namespace polymarket
