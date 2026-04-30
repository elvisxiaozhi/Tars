// 单元测试：Keccak-256 + Wallet (地址派生 + keystore 加解密)
//
// 运行：cmake --build build -j && ./build/test_crypto
// 退出码 0 = 全过；非 0 = 有失败

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include "core/polymarket_types.h"
#include "crypto/base64.h"
#include "crypto/hmac_sha256.h"
#include "crypto/keccak256.h"
#include "crypto/wallet.h"

using namespace polymarket;

static int g_passed = 0;
static int g_failed = 0;

#define ASSERT_EQ(actual, expected, name) do {                                    \
    if ((actual) == (expected)) {                                                 \
        std::cout << "  \033[32mPASS\033[0m  " << name << "\n";                   \
        ++g_passed;                                                               \
    } else {                                                                      \
        std::cout << "  \033[31mFAIL\033[0m  " << name << "\n"                    \
                  << "         actual:   " << (actual) << "\n"                    \
                  << "         expected: " << (expected) << "\n";                 \
        ++g_failed;                                                               \
    }                                                                             \
} while(0)

#define ASSERT_TRUE(cond, name) do {                                              \
    if (cond) {                                                                   \
        std::cout << "  \033[32mPASS\033[0m  " << name << "\n";                   \
        ++g_passed;                                                               \
    } else {                                                                      \
        std::cout << "  \033[31mFAIL\033[0m  " << name << " (cond false)\n";      \
        ++g_failed;                                                               \
    }                                                                             \
} while(0)

static std::string hashstr(const std::string& s) {
    return hex_encode(keccak256(s));
}

void test_keccak256() {
    std::cout << "[test_keccak256]\n";

    // KAT 1: empty
    ASSERT_EQ(hashstr(""),
              "c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470",
              "empty string");

    // KAT 2: "abc"
    ASSERT_EQ(hashstr("abc"),
              "4e03657aea45a94fc7d47ba826c8d667c0d1e6e33a64a036ec44f58fa12d6c45",
              "abc");

    // KAT 3: "The quick brown fox jumps over the lazy dog"
    ASSERT_EQ(hashstr("The quick brown fox jumps over the lazy dog"),
              "4d741b6f1eb29cb2a9b9911c82f56fa8d73b04959d3d9d222895df6c0b28aa15",
              "quick brown fox");

    // KAT 4-7: 边界长度（rate = 136 字节，前后各测一个）
    // 参考值由 pycryptodome 生成
    ASSERT_EQ(hashstr(std::string(135, 'a')),
              "34367dc248bbd832f4e3e69dfaac2f92638bd0bbd18f2912ba4ef454919cf446",
              "135×'a' (just under rate)");
    ASSERT_EQ(hashstr(std::string(136, 'a')),
              "a6c4d403279fe3e0af03729caada8374b5ca54d8065329a3ebcaeb4b60aa386e",
              "136×'a' (exactly one rate block)");
    ASSERT_EQ(hashstr(std::string(137, 'a')),
              "d869f639c7046b4929fc92a4d988a8b22c55fbadb802c0c66ebcd484f1915f39",
              "137×'a' (just over rate)");
    ASSERT_EQ(hashstr(std::string(200, 'a')),
              "96ea54061def936c4be90b518992fdc6f12f535068a256229aca54267b4d084d",
              "200×'a' (multi-block)");
}

void test_address_derivation() {
    std::cout << "\n[test_address_derivation]\n";

    // KAT: priv = 0x0000...0001 → 公认测试向量
    auto k1 = PrivateKey::from_hex(
        "0000000000000000000000000000000000000000000000000000000000000001");
    ASSERT_EQ(derive_address(k1),
              "0x7E5F4552091A69125d5DfCb7b8C2659029395Bdf",
              "privkey=1 → 0x7E5F4552...");

    // KAT: priv = 0x0000...0002
    auto k2 = PrivateKey::from_hex(
        "0000000000000000000000000000000000000000000000000000000000000002");
    ASSERT_EQ(derive_address(k2),
              "0x2B5AD5c4795c026514f8317c7a215E218DcCD6cF",
              "privkey=2 → 0x2B5AD5c4...");

    // KAT: priv = 0x0000...0003
    auto k3 = PrivateKey::from_hex(
        "0000000000000000000000000000000000000000000000000000000000000003");
    ASSERT_EQ(derive_address(k3),
              "0x6813Eb9362372EEF6200f3b1dbC3f819671cBA69",
              "privkey=3 → 0x6813Eb93...");
}

void test_keystore_roundtrip() {
    std::cout << "\n[test_keystore_roundtrip]\n";

    auto orig = PrivateKey::from_hex(
        "4c0883a69102937d6231471b5dbb6204fe5129617082792ae468d01a3f362318");
    std::string orig_addr = derive_address(orig);

    // 用低强度参数版本太慢，但 keystore 是 N=2^18 ~ 1s（mac M1），可接受
    std::string json = encrypt_keystore(orig, "test-password-123456");

    // 验证 JSON 结构
    ASSERT_TRUE(json.find("\"version\": 3") != std::string::npos, "JSON has version 3");
    ASSERT_TRUE(json.find("\"cipher\": \"aes-128-ctr\"") != std::string::npos, "cipher = aes-128-ctr");
    ASSERT_TRUE(json.find("\"kdf\": \"scrypt\"") != std::string::npos, "kdf = scrypt");

    // 正确密码 → 解密成功 + 私钥一致
    auto decrypted = decrypt_keystore(json, "test-password-123456");
    ASSERT_TRUE(decrypted.valid(), "decrypt succeeded");
    ASSERT_EQ(derive_address(decrypted), orig_addr, "decrypted privkey derives same address");

    // 错误密码 → 抛 MAC 异常
    bool threw = false;
    std::string err;
    try {
        (void)decrypt_keystore(json, "wrong-password");
    } catch (const std::exception& e) {
        threw = true;
        err = e.what();
    }
    ASSERT_TRUE(threw && err.find("MAC mismatch") != std::string::npos,
                "wrong password throws MAC mismatch");
}

void test_hex_helpers() {
    std::cout << "\n[test_hex_helpers]\n";

    uint8_t b[] = {0x00, 0xff, 0xab, 0xcd};
    ASSERT_EQ(hex_encode(b, 4), "00ffabcd", "encode");

    auto decoded = hex_decode("0x00ffabcd");
    ASSERT_TRUE(decoded.size() == 4 && decoded[0] == 0x00 && decoded[1] == 0xff &&
                decoded[2] == 0xab && decoded[3] == 0xcd,
                "decode with 0x prefix");

    bool threw = false;
    try { hex_decode("xyz"); } catch (...) { threw = true; }
    ASSERT_TRUE(threw, "decode invalid throws");

    threw = false;
    try { hex_decode("abc"); } catch (...) { threw = true; }  // odd length
    ASSERT_TRUE(threw, "decode odd-length throws");
}

// ── Base64 URL-safe (RFC 4648 §5) ───────────────────────────────────────────
static std::string b64u_str(const std::string& s) {
    return base64url_encode(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}
static std::string b64u_dec_str(const std::string& s) {
    auto b = base64url_decode(s);
    return std::string(b.begin(), b.end());
}

void test_base64url() {
    std::cout << "\n[test_base64url]\n";

    // RFC 4648 test vectors（字母表替换 +/ → -_，无 padding）
    ASSERT_EQ(b64u_str(""),       "",        "empty");
    ASSERT_EQ(b64u_str("f"),      "Zg",      "f");
    ASSERT_EQ(b64u_str("fo"),     "Zm8",     "fo");
    ASSERT_EQ(b64u_str("foo"),    "Zm9v",    "foo");
    ASSERT_EQ(b64u_str("foob"),   "Zm9vYg",  "foob");
    ASSERT_EQ(b64u_str("fooba"),  "Zm9vYmE", "fooba");
    ASSERT_EQ(b64u_str("foobar"), "Zm9vYmFy", "foobar");

    // url-safe 字符（标准 base64 中的 + / 在此处分别为 - _）
    uint8_t bytes_high[3] = {0xfb, 0xff, 0xbf};  // 标准 b64 = "-_-_"... 实测 "+/+/" 替换
    ASSERT_EQ(base64url_encode(bytes_high, 3), "-_-_", "edge bytes 0xfb/ff/bf → -_-_");

    // padded=true 与 Python urlsafe_b64encode 行为一致（保留 '='）
    auto enc_pad = [](const std::string& s) {
        return base64url_encode(reinterpret_cast<const uint8_t*>(s.data()), s.size(), /*padded=*/true);
    };
    ASSERT_EQ(enc_pad("f"),      "Zg==",     "padded: f → Zg==");
    ASSERT_EQ(enc_pad("fo"),     "Zm8=",     "padded: fo → Zm8=");
    ASSERT_EQ(enc_pad("foo"),    "Zm9v",     "padded: foo → Zm9v (no pad needed)");
    ASSERT_EQ(enc_pad("foobar"), "Zm9vYmFy", "padded: foobar → Zm9vYmFy");

    // 解码 round-trip
    ASSERT_EQ(b64u_dec_str("Zm9vYmFy"), "foobar", "decode foobar");
    ASSERT_EQ(b64u_dec_str("Zg"),       "f",      "decode 1-byte unpadded");
    ASSERT_EQ(b64u_dec_str("Zg=="),     "f",      "decode 1-byte with padding (tolerated)");
}

// ── HMAC-SHA256 (RFC 4231) ──────────────────────────────────────────────────
static std::string hmac_hex(const std::string& key, const std::string& msg) {
    auto h = hmac_sha256(reinterpret_cast<const uint8_t*>(key.data()), key.size(),
                         reinterpret_cast<const uint8_t*>(msg.data()), msg.size());
    return hex_encode(h);
}

void test_hmac_sha256() {
    std::cout << "\n[test_hmac_sha256]\n";

    // RFC 4231 Test Case 1
    std::string key1(20, '\x0b');
    ASSERT_EQ(hmac_hex(key1, "Hi There"),
              "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7",
              "RFC 4231 #1");

    // RFC 4231 Test Case 2
    ASSERT_EQ(hmac_hex("Jefe", "what do ya want for nothing?"),
              "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
              "RFC 4231 #2");

    // RFC 4231 Test Case 4 (key & data > 20 bytes)
    std::string key4;
    for (int i = 1; i <= 25; ++i) key4 += static_cast<char>(i);
    std::string data4(50, '\xcd');
    ASSERT_EQ(hmac_hex(key4, data4),
              "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b",
              "RFC 4231 #4");
}

// ── L2 build_hmac_l2 集成 ────────────────────────────────────────────────────
void test_build_hmac_l2() {
    std::cout << "\n[test_build_hmac_l2]\n";

    // 已知组合：secret 是 url-safe base64("secret-key")，期望签名 = base64url(HMAC-SHA256(raw_key, "1714478400GET/balance-allowance"))
    std::string secret_b64 = b64u_str("secret-key");

    // 直接对照算预期值
    std::string msg = std::string("1714478400") + "GET" + "/balance-allowance";
    auto expected_h = hmac_sha256(
        reinterpret_cast<const uint8_t*>("secret-key"), 10,
        reinterpret_cast<const uint8_t*>(msg.data()), msg.size());
    // build_hmac_l2 输出带 padding（与 py-clob-client 一致），KAT 期望也带
    std::string expected = base64url_encode(expected_h.data(), expected_h.size(), /*padded=*/true);

    std::string got = build_hmac_l2(secret_b64, "1714478400", "GET", "/balance-allowance");
    ASSERT_EQ(got, expected, "L2 sig matches base64url(HMAC-SHA256(raw_secret, ts||method||path))");

    // body 中单引号要标准化为双引号（py-clob-client 怪癖）
    std::string with_body = build_hmac_l2(
        secret_b64, "1", "POST", "/order", "{'foo': 'bar'}");
    std::string no_quote_body = build_hmac_l2(
        secret_b64, "1", "POST", "/order", "{\"foo\": \"bar\"}");
    ASSERT_EQ(with_body, no_quote_body, "body single→double quote normalized");
}

int main() {
    std::cout << "===== crypto unit tests =====\n\n";

    test_hex_helpers();
    test_keccak256();
    test_address_derivation();
    test_keystore_roundtrip();
    test_base64url();
    test_hmac_sha256();
    test_build_hmac_l2();

    std::cout << "\n===== "
              << g_passed << " passed, " << g_failed << " failed =====\n";
    return g_failed == 0 ? 0 : 1;
}
