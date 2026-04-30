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
                 "ClobAuth(address address,uint256 timestamp,uint256 nonce,string message)"));

        EIP712Domain poly_domain{
            "Polymarket CTF Exchange", "1", 137,
            "0x4bFb41d5B3570DeFd03C39a9A4D8dE6Bd8B8982E"};
        info("Polymarket CTFExchange domain separator",
             eip712_domain_separator(poly_domain));

        EIP712Domain clob_domain{"ClobAuthDomain", "1", 137, ""};
        info("ClobAuthDomain separator", eip712_domain_separator(clob_domain));
    }

    // ── 5. sign_poly_order smoke test ─────────────────────────────────────────
    std::cout << "\n[5] sign_poly_order smoke test\n";
    {
        auto key = PrivateKey::from_hex(
            "9c22ff5f21f0b81b113e63f7db6da94fedef11b2119b4088b89664fb9a3cb658");

        PolyOrder order{};
        order.salt           = 12345678;
        order.maker          = "0xCD2a3d9F938E13CD947Ec05AbC7FE734Df8DD826";
        order.taker          = "0x0000000000000000000000000000000000000000";
        order.token_id       =
            "52114319501245915516055106046884209969926127482827954674443846427813813222426";
        order.maker_amount   = 5'000'000;  // $5.00 USDC
        order.taker_amount   = 5'000'000;  // 5.00 shares @ $1.00 implied
        order.expiration     = 0;
        order.nonce          = 0;
        order.fee_rate_bps   = 0;
        order.side           = 0;  // BUY
        order.signature_type = 0;  // EOA

        auto sig = sign_poly_order(key, order);
        std::cout << "  sig: " << sig.to_hex() << "\n";
        if (sig.v == 27 || sig.v == 28) {
            std::cout << "  [OK]   v = " << (int)sig.v << " (27 or 28)\n";
        } else {
            std::cout << "  [FAIL] v = " << (int)sig.v << "\n";
            ++g_failures;
        }
    }

    std::cout << "\n=== " << (g_failures == 0 ? "ALL PASS" : std::to_string(g_failures) + " FAILURE(S)") << " ===\n";
    return g_failures > 0 ? 1 : 0;
}
