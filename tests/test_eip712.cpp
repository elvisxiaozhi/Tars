// test_eip712 — KAT suite for the EIP-712 library
//
// Reference values: EIP-712 proposal Mail example (eip712.org)
//   domainSeparator  = 0xf2cee375fa42b42143804025fc449deafd50cc031ca257e0b194a650a912090f
//   hashStruct(mail) = 0xc52c0ee5d84264471806290a3f2c4cecfc5490626bf912d01f240d7a274b371e
//   final digest     = 0xbe609aee343fb3c4b28e1df9e632fca64fcfaede20f02e86244efddf30957bd2
//
// Usage: ./build/test_eip712

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "core/polymarket_types.h"
#include "crypto/eip712.h"
#include "crypto/keccak256.h"
#include "crypto/wallet.h"

using namespace polymarket;

static int g_failures = 0;

static void check(const std::string& label,
                  const std::array<uint8_t, 32>& got,
                  const std::string& expected_hex_no_prefix) {
    std::string got_hex = hex_encode(got);
    if (got_hex == expected_hex_no_prefix) {
        std::cout << "  [OK]   " << label << "\n";
    } else {
        std::cout << "  [FAIL] " << label << "\n"
                  << "         expected: " << expected_hex_no_prefix << "\n"
                  << "         got:      " << got_hex << "\n";
        ++g_failures;
    }
}

static void info(const std::string& label, const std::array<uint8_t, 32>& h) {
    std::cout << "  [info] " << label << ": " << hex_encode(h) << "\n";
}

int main() {
    std::cout << "=== test_eip712 ===\n\n";

    // ── 1. Encoding primitives ────────────────────────────────────────────────
    std::cout << "[1] Encoding primitives\n";

    check("uint256(0)",
          eip712_uint256(0),
          std::string(64, '0'));

    check("uint256(1)",
          eip712_uint256(1),
          std::string(63, '0') + "1");

    check("uint256(0xDEADBEEF)",
          eip712_uint256(0xDEAD'BEEF),
          "00000000000000000000000000000000000000000000000000000000deadbeef");

    check("uint256_str(\"0\")",
          eip712_uint256_str("0"),
          std::string(64, '0'));

    check("uint256_str(\"256\")",
          eip712_uint256_str("256"),
          "0000000000000000000000000000000000000000000000000000000000000100");

    check("uint256_str(\"1000000\") = 0xf4240",
          eip712_uint256_str("1000000"),
          "00000000000000000000000000000000000000000000000000000000000f4240");

    // Large Polymarket tokenId (example)
    info("uint256_str(large tokenId)",
         eip712_uint256_str(
             "52114319501245915516055106046884209969926127482827954674443846427813813222426"));

    check("address(0xCcCC…) left-padded",
          eip712_address("0xCcCCccccCCCCcCCCCCCcCcCccCcCCCcCcccccccC"),
          "000000000000000000000000cccccccccccccccccccccccccccccccccccccccc");

    check("bool(true)",
          eip712_bool(true),
          "0000000000000000000000000000000000000000000000000000000000000001");

    check("bool(false)",
          eip712_bool(false),
          std::string(64, '0'));

    // ── 2. EIP-712 Mail example (from EIP proposal) ───────────────────────────
    std::cout << "\n[2] EIP-712 Mail example (reference: EIP-712 proposal)\n";

    const std::string PERSON_TYPE = "Person(string name,address wallet)";
    // Deps appended alphabetically after primary type
    const std::string MAIL_TYPE_FULL =
        "Mail(Person from,Person to,string contents)"
        "Person(string name,address wallet)";

    auto person_th = eip712_type_hash(PERSON_TYPE);
    auto mail_th   = eip712_type_hash(MAIL_TYPE_FULL);
    info("Person typeHash", person_th);
    info("Mail typeHash",   mail_th);

    // Domain: { name:"Ether Mail", version:"1", chainId:1, verifyingContract:0xCcCC… }
    EIP712Domain domain;
    domain.name               = "Ether Mail";
    domain.version            = "1";
    domain.chain_id           = 1;
    domain.verifying_contract = "0xCcCCccccCCCCcCCCCCCcCcCccCcCCCcCcccccccC";

    auto dom_sep = eip712_domain_separator(domain);
    check("domainSeparator",
          dom_sep,
          "f2cee375fa42b42143804025fc449deafd50cc031ca257e0b194a650a912090f");

    // from: { name:"Cow", wallet:0xCD2a3d9F938E13CD947Ec05AbC7FE734Df8DD826 }
    auto from_hash = eip712_struct_hash(person_th, {
        eip712_string("Cow"),
        eip712_address("0xCD2a3d9F938E13CD947Ec05AbC7FE734Df8DD826"),
    });
    info("hashStruct(from)", from_hash);

    // to: { name:"Bob", wallet:0xbBbBBBBbbBBBbbbBbbBbbbbBBbBbbbbBbBbbBBbB }
    auto to_hash = eip712_struct_hash(person_th, {
        eip712_string("Bob"),
        eip712_address("0xbBbBBBBbbBBBbbbBbbBbbbbBBbBbbbbBbBbbBBbB"),
    });
    info("hashStruct(to)", to_hash);

    // mail: { from, to, contents:"Hello, Bob!" }
    // Nested struct field value = its own structHash
    auto mail_hash = eip712_struct_hash(mail_th, {
        from_hash,                       // nested Person → pass structHash directly
        to_hash,
        eip712_string("Hello, Bob!"),
    });
    check("hashStruct(mail)",
          mail_hash,
          "c52c0ee5d84264471806290a3f2c4cecfc5490626bf912d01f240d7a274b371e");

    auto final_digest = eip712_encode(dom_sep, mail_hash);
    check("final digest (Mail)",
          final_digest,
          "be609aee343fb3c4b28e1df9e632fca64fcfaede20f02e86244efddf30957bd2");

    // ── 3. Signing — v must be 27 or 28 ──────────────────────────────────────
    std::cout << "\n[3] ECDSA sign\n";
    {
        // Test key: keccak256("test") — never used in production
        auto key = PrivateKey::from_hex(
            "9c22ff5f21f0b81b113e63f7db6da94fedef11b2119b4088b89664fb9a3cb658");
        auto sig = eip712_sign(key, final_digest);
        std::cout << "  sig: " << sig.to_hex() << "\n";
        if (sig.v == 27 || sig.v == 28) {
            std::cout << "  [OK]   v = " << (int)sig.v << " (27 or 28)\n";
        } else {
            std::cout << "  [FAIL] v = " << (int)sig.v << " (expected 27 or 28)\n";
            ++g_failures;
        }
    }

    // ── 4. Polymarket type hashes (print for cross-check against py-clob-client) ──
    std::cout << "\n[4] Polymarket type hashes (verify with py-clob-client)\n";
    {
        info("Order typeHash",
             eip712_type_hash(
                 "Order(uint256 salt,address maker,address taker,uint256 tokenId,"
                 "uint256 makerAmount,uint256 takerAmount,uint256 expiration,"
                 "uint256 nonce,uint256 feeRateBps,uint256 side,uint256 signatureType)"));

        info("ClobAuth typeHash",
             eip712_type_hash(
                 "ClobAuth(address address,string timestamp,uint256 nonce,string message)"));

        // V2 Order typehash KAT — 必须 == ctf-exchange-v2/Structs.sol ORDER_TYPEHASH 常量
        check("V2 Order typehash matches on-chain constant",
              eip712_type_hash(
                  "Order(uint256 salt,address maker,address signer,uint256 tokenId,"
                  "uint256 makerAmount,uint256 takerAmount,uint8 side,uint8 signatureType,"
                  "uint256 timestamp,bytes32 metadata,bytes32 builder)"),
              "bb86318a2138f5fa8ae32fbe8e659f8fcf13cc6ae4014a707893055433818589");

        EIP712Domain poly_domain_v2{
            "Polymarket CTF Exchange", "2", 137,
            "0xE111180000d2663C0091e4f400237545B87B996B"};
        info("V2 Polymarket CTFExchange domain separator",
             eip712_domain_separator(poly_domain_v2));

        EIP712Domain clob_domain{"ClobAuthDomain", "1", 137, ""};
        info("ClobAuthDomain separator", eip712_domain_separator(clob_domain));
    }

    // ── 5. sign_poly_order V2 KAT (vs py-clob-client-v2) ─────────────────────
    // 黄金对照：tools/dry_sign.py 在固定输入下用 py-clob-client-v2 SDK 签出的真实签名。
    // 输入：EOA priv = 0x000...001（→ 0x7E5F4552...），funder = proxy，BUY 10 shares @ $0.50
    std::cout << "\n[5] sign_poly_order V2 KAT (matches py-clob-client-v2)\n";
    {
        auto key = PrivateKey::from_hex(
            "0000000000000000000000000000000000000000000000000000000000000001");

        PolyOrder order{};
        order.salt           = 348624930908ULL;
        order.maker          = "0x356E4c0a80B5Ac2466B7a62A11B35e8DbC7d2196";
        order.signer         = "0x7E5F4552091A69125d5DfCb7b8C2659029395Bdf";
        order.token_id       =
            "52114319501245915516055106046884209969926127482827954674443846427813813222426";
        order.maker_amount   = 5'000'000ULL;   // size × price × 1e6 = 10 × 0.5 × 1e6
        order.taker_amount   = 10'000'000ULL;  // size × 1e6 = 10 × 1e6
        order.side           = 0;              // BUY
        order.signature_type = 1;              // POLY_PROXY
        order.timestamp      = 1777568848108ULL;  // ms
        // metadata / builder 默认全 0

        auto sig = sign_poly_order(key, order);
        std::string got = sig.to_hex();
        std::string expected_sig =
            "0xfa1e8a63c7c23909290c67b5db3e8054ac1bbaf9edba9584c2f2021de9cd1d37"
            "709ed5c1bb0c60d2c9f24b69c6937c34667a799380c1e4d4d33ac75240c4159d1c";

        if (got == expected_sig) {
            std::cout << "  [OK]   sig matches py-clob-client-v2 dry-sign output\n";
            std::cout << "         " << got << "\n";
        } else {
            std::cout << "  [FAIL] sig mismatch\n"
                      << "         expected: " << expected_sig << "\n"
                      << "         got:      " << got << "\n";
            ++g_failures;
        }

        // ── polyorder_to_json KAT —— 字节级匹配 py-clob-client-v2 / order_to_json_v2 ──
        // 关键：salt/signatureType 是 INT(无引号)，side 是 string "BUY"，
        // 字段顺序按 SDK：side → expiration → signatureType（不是 side → signatureType → ... → expiration）
        std::string json = polyorder_to_json(order, sig);
        std::string expected_json =
            "{\"salt\":348624930908,"
            "\"maker\":\"0x356E4c0a80B5Ac2466B7a62A11B35e8DbC7d2196\","
            "\"signer\":\"0x7E5F4552091A69125d5DfCb7b8C2659029395Bdf\","
            "\"tokenId\":\"52114319501245915516055106046884209969926127482827954674443846427813813222426\","
            "\"makerAmount\":\"5000000\","
            "\"takerAmount\":\"10000000\","
            "\"side\":\"BUY\","
            "\"expiration\":\"0\","
            "\"signatureType\":1,"
            "\"timestamp\":\"1777568848108\","
            "\"metadata\":\"0x0000000000000000000000000000000000000000000000000000000000000000\","
            "\"builder\":\"0x0000000000000000000000000000000000000000000000000000000000000000\","
            "\"signature\":\"0xfa1e8a63c7c23909290c67b5db3e8054ac1bbaf9edba9584c2f2021de9cd1d37709ed5c1bb0c60d2c9f24b69c6937c34667a799380c1e4d4d33ac75240c4159d1c\"}";
        if (json == expected_json) {
            std::cout << "  [OK]   compact JSON matches dry-sign output\n";
        } else {
            std::cout << "  [FAIL] JSON mismatch\n"
                      << "         expected: " << expected_json << "\n"
                      << "         got:      " << json << "\n";
            ++g_failures;
        }
    }

    std::cout << "\n=== " << (g_failures == 0 ? "ALL PASS" : std::to_string(g_failures) + " FAILURE(S)") << " ===\n";
    return g_failures > 0 ? 1 : 0;
}
