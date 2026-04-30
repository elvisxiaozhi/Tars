#include "core/polymarket_types.h"

#include <vector>

#include "crypto/eip712.h"
#include "crypto/wallet.h"

namespace polymarket {

// CTFExchange V2 (Polygon mainnet, 2026-04-28 升级)
// NegRisk Exchange = 0xe2222d279d744050d28e00520010520000310F59（暂未使用）
static const char* EXCHANGE_ADDR = "0xE111180000d2663C0091e4f400237545B87B996B";

// V2 type string — typehash KAT 已在 test_eip712 中验证 == 链上 ORDER_TYPEHASH
//   0xbb86318a2138f5fa8ae32fbe8e659f8fcf13cc6ae4014a707893055433818589
// side 与 signatureType 在 Solidity 中是 enum（uint8），EIP-712 编码同 uint256（32 字节右对齐）
static const char* ORDER_TYPE =
    "Order(uint256 salt,address maker,address signer,uint256 tokenId,"
    "uint256 makerAmount,uint256 takerAmount,uint8 side,uint8 signatureType,"
    "uint256 timestamp,bytes32 metadata,bytes32 builder)";

// side and signatureType are uint256 in the Polymarket ABI (not uint8)
// timestamp 是 String 类型（py-clob-client 用 poly-eip712-structs.String()），
// 编码为 keccak256(decimal_string)；POLY_TIMESTAMP header 必须是同一个十进制串
static const char* CLOB_AUTH_TYPE =
    "ClobAuth(address address,string timestamp,uint256 nonce,string message)";

Signature sign_poly_order(const PrivateKey& key, const PolyOrder& order, uint64_t chain_id) {
    EIP712Domain domain{"Polymarket CTF Exchange", "2", chain_id, EXCHANGE_ADDR};

    auto th = eip712_type_hash(ORDER_TYPE);
    std::vector<std::array<uint8_t, 32>> fields = {
        eip712_uint256(order.salt),
        eip712_address(order.maker),
        eip712_address(order.signer),
        eip712_uint256_str(order.token_id),
        eip712_uint256(order.maker_amount),
        eip712_uint256(order.taker_amount),
        eip712_uint256(order.side),
        eip712_uint256(order.signature_type),
        eip712_uint256(order.timestamp),
        eip712_bytes32(order.metadata),
        eip712_bytes32(order.builder),
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
        eip712_string(std::to_string(auth.timestamp)),
        eip712_uint256(auth.nonce),
        eip712_string(auth.message),
    };

    auto sh     = eip712_struct_hash(th, fields);
    auto ds     = eip712_domain_separator(domain);
    auto digest = eip712_encode(ds, sh);
    return eip712_sign(key, digest);
}

}  // namespace polymarket
