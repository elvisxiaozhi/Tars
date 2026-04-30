#include "core/polymarket_types.h"

#include <vector>

#include "crypto/eip712.h"
#include "crypto/wallet.h"

namespace polymarket {

// CTFExchange (Polygon mainnet)
static const char* EXCHANGE_ADDR = "0x4bFb41d5B3570DeFd03C39a9A4D8dE6Bd8B8982E";

// Full type strings (no spaces between field tuples — EIP-712 canonical form)
static const char* ORDER_TYPE =
    "Order(uint256 salt,address maker,address taker,uint256 tokenId,"
    "uint256 makerAmount,uint256 takerAmount,uint256 expiration,"
    "uint256 nonce,uint256 feeRateBps,uint256 side,uint256 signatureType)";

// side and signatureType are uint256 in the Polymarket ABI (not uint8)
static const char* CLOB_AUTH_TYPE =
    "ClobAuth(address address,uint256 timestamp,uint256 nonce,string message)";

Signature sign_poly_order(const PrivateKey& key, const PolyOrder& order, uint64_t chain_id) {
    EIP712Domain domain{"Polymarket CTF Exchange", "1", chain_id, EXCHANGE_ADDR};

    auto th = eip712_type_hash(ORDER_TYPE);
    std::vector<std::array<uint8_t, 32>> fields = {
        eip712_uint256(order.salt),
        eip712_address(order.maker),
        eip712_address(order.taker),
        eip712_uint256_str(order.token_id),
        eip712_uint256(order.maker_amount),
        eip712_uint256(order.taker_amount),
        eip712_uint256(order.expiration),
        eip712_uint256(order.nonce),
        eip712_uint256(order.fee_rate_bps),
        eip712_uint256(order.side),
        eip712_uint256(order.signature_type),
    };

    auto sh     = eip712_struct_hash(th, fields);
    auto ds     = eip712_domain_separator(domain);
    auto digest = eip712_encode(ds, sh);
    return eip712_sign(key, digest);
}

Signature sign_clob_auth(const PrivateKey& key, const ClobAuthData& auth, uint64_t chain_id) {
    // ClobAuthDomain has no verifyingContract
    EIP712Domain domain{"ClobAuthDomain", "1", chain_id, ""};

    auto th = eip712_type_hash(CLOB_AUTH_TYPE);
    std::vector<std::array<uint8_t, 32>> fields = {
        eip712_address(auth.address),
        eip712_uint256(auth.timestamp),
        eip712_uint256(auth.nonce),
        eip712_string(auth.message),
    };

    auto sh     = eip712_struct_hash(th, fields);
    auto ds     = eip712_domain_separator(domain);
    auto digest = eip712_encode(ds, sh);
    return eip712_sign(key, digest);
}

}  // namespace polymarket
