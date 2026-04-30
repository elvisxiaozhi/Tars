#include "core/polymarket_types.h"

#include <vector>

#include "crypto/base64.h"
#include "crypto/eip712.h"
#include "crypto/hmac_sha256.h"
#include "crypto/keccak256.h"
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

// L2 HMAC 签名 — 与 py-clob-client/signing/hmac.py 行为对齐：
//   1. base64url-decode api_secret  → HMAC key
//   2. message = ts || method || path  (+ body, body 中单引号 → 双引号)
//   3. HMAC-SHA256(key, message)  → base64url-encode(no pad) = POLY_SIGNATURE
std::string build_hmac_l2(const std::string& api_secret_b64,
                          const std::string& timestamp,
                          const std::string& method,
                          const std::string& request_path,
                          const std::string& body) {
    auto key = base64url_decode(api_secret_b64);

    std::string message;
    message.reserve(timestamp.size() + method.size() + request_path.size() + body.size());
    message.append(timestamp);
    message.append(method);
    message.append(request_path);
    if (!body.empty()) {
        std::string b = body;
        for (auto& c : b) if (c == '\'') c = '"';
        message.append(b);
    }

    auto sig = hmac_sha256(key.data(), key.size(),
                           reinterpret_cast<const uint8_t*>(message.data()),
                           message.size());
    // padded=true：与 py-clob-client 的 urlsafe_b64encode 行为对齐（保留 '=' padding）
    return base64url_encode(sig.data(), sig.size(), /*padded=*/true);
}

// 手撸紧凑 JSON serializer — 字节级匹配 py-clob-client-v2 / order_to_json_v2 输出。
// 关键不变量（与 SDK 源码 client.py:order_to_json_v2 对齐）：
//   字段顺序: salt, maker, signer, tokenId, makerAmount, takerAmount,
//             side, expiration, signatureType, timestamp,
//             metadata, builder, signature
//   值类型: salt / signatureType  → INT（无引号）
//           side                  → 字符串 "BUY"/"SELL"（不是 0/1）
//           其他 amount/timestamp → 字符串
std::string polyorder_to_json(const PolyOrder& order, const Signature& sig,
                              const std::string& expiration_str) {
    std::string out;
    out.reserve(900);
    out += '{';
    out += "\"salt\":";            out += std::to_string(order.salt);                        out += ",";
    out += "\"maker\":\"";         out += order.maker;                                       out += "\",";
    out += "\"signer\":\"";        out += order.signer;                                      out += "\",";
    out += "\"tokenId\":\"";       out += order.token_id;                                    out += "\",";
    out += "\"makerAmount\":\"";   out += std::to_string(order.maker_amount);                out += "\",";
    out += "\"takerAmount\":\"";   out += std::to_string(order.taker_amount);                out += "\",";
    out += "\"side\":\"";          out += (order.side == 0 ? "BUY" : "SELL");                out += "\",";
    out += "\"expiration\":\"";    out += expiration_str;                                    out += "\",";
    out += "\"signatureType\":";   out += std::to_string(static_cast<int>(order.signature_type));  out += ",";
    out += "\"timestamp\":\"";     out += std::to_string(order.timestamp);                   out += "\",";
    out += "\"metadata\":\"0x";    out += hex_encode(order.metadata.data(), 32);             out += "\",";
    out += "\"builder\":\"0x";     out += hex_encode(order.builder.data(),  32);             out += "\",";
    out += "\"signature\":\"";     out += sig.to_hex();                                      out += '"';
    out += '}';
    return out;
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
