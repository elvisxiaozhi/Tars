#pragma once

// Polymarket-specific EIP-712 structs and signing helpers.
// Generic EIP-712 primitives live in crypto/eip712.h.

#include <cstdint>
#include <string>

namespace polymarket {

class PrivateKey;
struct Signature;

// ── CTFExchange Order ─────────────────────────────────────────────────────────

struct PolyOrder {
    uint64_t    salt;           // random nonce
    std::string maker;          // EOA "0x…" (signs the order)
    std::string taker;          // "0x0000…" for open orders
    std::string token_id;       // binary outcome token ID — large uint256 as decimal string
    uint64_t    maker_amount;   // USDC micro-units (6 decimals); 5_000_000 = $5.00
    uint64_t    taker_amount;   // shares micro-units
    uint64_t    expiration;     // unix seconds (0 = no expiry)
    uint64_t    nonce;          // usually 0
    uint64_t    fee_rate_bps;   // usually 0 (maker)
    uint8_t     side;           // 0 = BUY, 1 = SELL
    uint8_t     signature_type; // 0 = EOA, 1 = POLY_PROXY, 2 = POLY_GNOSIS_SAFE
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

// ── Signing helpers ───────────────────────────────────────────────────────────

// Sign a Polymarket limit order on CTFExchange (Polygon mainnet, chain 137).
Signature sign_poly_order(const PrivateKey& key, const PolyOrder& order,
                          uint64_t chain_id = 137);

// Sign CLOB API authentication on ClobAuthDomain.
Signature sign_clob_auth(const PrivateKey& key, const ClobAuthData& auth,
                         uint64_t chain_id = 137);

}  // namespace polymarket
