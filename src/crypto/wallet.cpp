#include "crypto/wallet.h"

#include <cstring>
#include <stdexcept>
#include <vector>

#include <json.hpp>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/kdf.h>
#include <openssl/core_names.h>
#include <openssl/params.h>
#include <secp256k1.h>

#include "crypto/keccak256.h"

namespace polymarket {

void secure_zero(void* ptr, size_t len) {
    // 用 volatile 函数指针绕过编译器死代码消除
    static void* (*const volatile memset_v)(void*, int, size_t) = std::memset;
    memset_v(ptr, 0, len);
}

// ================== PrivateKey ==================

PrivateKey::PrivateKey(const uint8_t bytes[32]) {
    std::memcpy(bytes_, bytes, 32);
    valid_ = true;
}

PrivateKey::~PrivateKey() {
    secure_zero(bytes_, 32);
    valid_ = false;
}

PrivateKey::PrivateKey(PrivateKey&& other) noexcept {
    std::memcpy(bytes_, other.bytes_, 32);
    valid_ = other.valid_;
    secure_zero(other.bytes_, 32);
    other.valid_ = false;
}

PrivateKey& PrivateKey::operator=(PrivateKey&& other) noexcept {
    if (this != &other) {
        secure_zero(bytes_, 32);
        std::memcpy(bytes_, other.bytes_, 32);
        valid_ = other.valid_;
        secure_zero(other.bytes_, 32);
        other.valid_ = false;
    }
    return *this;
}

PrivateKey PrivateKey::from_hex(const std::string& hex) {
    auto bytes = hex_decode(hex);
    if (bytes.size() != 32) {
        throw std::invalid_argument(
            "PrivateKey::from_hex: expected 32 bytes (64 hex chars), got " +
            std::to_string(bytes.size()));
    }
    PrivateKey k;
    std::memcpy(k.bytes_, bytes.data(), 32);
    k.valid_ = true;
    secure_zero(bytes.data(), bytes.size());
    return k;
}

// ================== Address derivation ==================

std::string derive_address(const PrivateKey& key) {
    if (!key.valid()) throw std::runtime_error("derive_address: invalid PrivateKey");

    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
    if (!ctx) throw std::runtime_error("secp256k1_context_create failed");

    // 校验私钥（必须在曲线阶范围内）
    if (!secp256k1_ec_seckey_verify(ctx, key.data())) {
        secp256k1_context_destroy(ctx);
        throw std::runtime_error("derive_address: invalid private key (out of curve order)");
    }

    secp256k1_pubkey pubkey;
    if (!secp256k1_ec_pubkey_create(ctx, &pubkey, key.data())) {
        secp256k1_context_destroy(ctx);
        throw std::runtime_error("secp256k1_ec_pubkey_create failed");
    }

    uint8_t pub_serialized[65];
    size_t pub_len = 65;
    secp256k1_ec_pubkey_serialize(ctx, pub_serialized, &pub_len,
                                  &pubkey, SECP256K1_EC_UNCOMPRESSED);
    secp256k1_context_destroy(ctx);
    if (pub_len != 65 || pub_serialized[0] != 0x04) {
        throw std::runtime_error("unexpected pubkey serialization");
    }

    // keccak256 over 64-byte raw pubkey (drop leading 0x04)
    auto hash = keccak256(pub_serialized + 1, 64);

    // Lowercase hex of last 20 bytes
    std::string addr_lower = hex_encode(hash.data() + 12, 20);

    // EIP-55 mixed-case checksum
    auto check_hash = keccak256(addr_lower);
    std::string out = "0x";
    for (size_t i = 0; i < 40; ++i) {
        char c = addr_lower[i];
        if (c >= 'a' && c <= 'f') {
            int nibble = (i % 2 == 0)
                ? (check_hash[i / 2] >> 4) & 0xf
                : check_hash[i / 2] & 0xf;
            if (nibble >= 8) c = static_cast<char>(c - 'a' + 'A');
        }
        out.push_back(c);
    }
    return out;
}

// ================== scrypt helper ==================

namespace {

void scrypt_kdf(const std::string& password,
                const uint8_t* salt, size_t salt_len,
                uint64_t N, uint64_t r, uint64_t p,
                uint8_t* out, size_t dklen) {
    EVP_KDF* kdf = EVP_KDF_fetch(nullptr, "SCRYPT", nullptr);
    if (!kdf) throw std::runtime_error("EVP_KDF_fetch(SCRYPT) failed");

    EVP_KDF_CTX* kctx = EVP_KDF_CTX_new(kdf);
    EVP_KDF_free(kdf);
    if (!kctx) throw std::runtime_error("EVP_KDF_CTX_new failed");

    // scrypt 默认有内存上限 1GB；N=2^18, r=8 需要 ~512MB，要显式抬阈值
    uint64_t maxmem = 2ULL * 1024 * 1024 * 1024;  // 2 GB

    OSSL_PARAM params[7];
    int idx = 0;
    params[idx++] = OSSL_PARAM_construct_octet_string(
        OSSL_KDF_PARAM_PASSWORD, const_cast<char*>(password.data()), password.size());
    params[idx++] = OSSL_PARAM_construct_octet_string(
        OSSL_KDF_PARAM_SALT, const_cast<uint8_t*>(salt), salt_len);
    params[idx++] = OSSL_PARAM_construct_uint64(OSSL_KDF_PARAM_SCRYPT_N, &N);
    params[idx++] = OSSL_PARAM_construct_uint64(OSSL_KDF_PARAM_SCRYPT_R, &r);
    params[idx++] = OSSL_PARAM_construct_uint64(OSSL_KDF_PARAM_SCRYPT_P, &p);
    params[idx++] = OSSL_PARAM_construct_uint64(OSSL_KDF_PARAM_SCRYPT_MAXMEM, &maxmem);
    params[idx++] = OSSL_PARAM_construct_end();

    if (EVP_KDF_derive(kctx, out, dklen, params) != 1) {
        EVP_KDF_CTX_free(kctx);
        throw std::runtime_error("scrypt derivation failed");
    }
    EVP_KDF_CTX_free(kctx);
}

void aes_128_ctr(const uint8_t key[16], const uint8_t iv[16],
                 const uint8_t* in, uint8_t* out, size_t len) {
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) throw std::runtime_error("EVP_CIPHER_CTX_new failed");
    if (EVP_EncryptInit_ex(ctx, EVP_aes_128_ctr(), nullptr, key, iv) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("EVP_EncryptInit_ex failed");
    }
    int outlen = 0;
    if (EVP_EncryptUpdate(ctx, out, &outlen, in, static_cast<int>(len)) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("EVP_EncryptUpdate failed");
    }
    int finallen = 0;
    if (EVP_EncryptFinal_ex(ctx, out + outlen, &finallen) != 1) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("EVP_EncryptFinal_ex failed");
    }
    EVP_CIPHER_CTX_free(ctx);
}

void random_bytes(uint8_t* buf, size_t len) {
    if (RAND_bytes(buf, static_cast<int>(len)) != 1) {
        throw std::runtime_error("RAND_bytes failed");
    }
}

bool constant_time_eq(const uint8_t* a, const uint8_t* b, size_t len) {
    uint8_t diff = 0;
    for (size_t i = 0; i < len; ++i) diff |= static_cast<uint8_t>(a[i] ^ b[i]);
    return diff == 0;
}

std::string make_uuid_v4() {
    uint8_t u[16];
    random_bytes(u, 16);
    u[6] = (u[6] & 0x0f) | 0x40;  // version 4
    u[8] = (u[8] & 0x3f) | 0x80;  // RFC 4122 variant
    char buf[37];
    std::snprintf(buf, sizeof(buf),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                  u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7],
                  u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
    return std::string(buf);
}

}  // namespace

// ================== encrypt_keystore ==================

std::string encrypt_keystore(const PrivateKey& key, const std::string& password) {
    if (!key.valid()) throw std::runtime_error("encrypt_keystore: invalid PrivateKey");
    if (password.empty()) throw std::runtime_error("encrypt_keystore: empty password");

    // 1. random salt + iv
    uint8_t salt[32], iv[16];
    random_bytes(salt, 32);
    random_bytes(iv, 16);

    // 2. scrypt(N=2^18, r=8, p=1) → 32 字节 derived key
    constexpr uint64_t N = 1ULL << 18;  // 262144
    constexpr uint64_t r = 8;
    constexpr uint64_t p = 1;
    constexpr int dklen = 32;
    uint8_t derived[32];
    scrypt_kdf(password, salt, 32, N, r, p, derived, dklen);

    // 3. AES-128-CTR encrypt with derived[0..16]
    uint8_t ciphertext[32];
    aes_128_ctr(derived, iv, key.data(), ciphertext, 32);

    // 4. MAC = keccak256(derived[16..32] || ciphertext)
    uint8_t mac_input[16 + 32];
    std::memcpy(mac_input, derived + 16, 16);
    std::memcpy(mac_input + 16, ciphertext, 32);
    auto mac = keccak256(mac_input, sizeof(mac_input));

    // 5. 派生地址（写入 keystore 元数据）
    std::string address = derive_address(key);
    // keystore 约定 address 字段不带 0x、且小写
    std::string addr_lower;
    addr_lower.reserve(40);
    for (size_t i = 2; i < address.size(); ++i) {
        char c = address[i];
        if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
        addr_lower.push_back(c);
    }

    // 6. JSON
    nlohmann::json j;
    j["version"] = 3;
    j["id"] = make_uuid_v4();
    j["address"] = addr_lower;
    j["crypto"] = {
        {"ciphertext", hex_encode(ciphertext, 32)},
        {"cipherparams", {{"iv", hex_encode(iv, 16)}}},
        {"cipher", "aes-128-ctr"},
        {"kdf", "scrypt"},
        {"kdfparams", {
            {"dklen", dklen},
            {"salt", hex_encode(salt, 32)},
            {"n", N},
            {"r", r},
            {"p", p}
        }},
        {"mac", hex_encode(mac.data(), 32)}
    };

    // 清擦
    secure_zero(derived, 32);
    secure_zero(mac_input, sizeof(mac_input));

    return j.dump(2);
}

// ================== decrypt_keystore ==================

PrivateKey decrypt_keystore(const std::string& json_str, const std::string& password) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(json_str);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("decrypt_keystore: invalid JSON: ") + e.what());
    }

    if (!j.contains("version") || j["version"].get<int>() != 3) {
        throw std::runtime_error("decrypt_keystore: unsupported version (expected 3)");
    }
    auto& crypto = j.at("crypto");
    std::string cipher = crypto.at("cipher").get<std::string>();
    std::string kdf = crypto.at("kdf").get<std::string>();
    if (cipher != "aes-128-ctr") {
        throw std::runtime_error("decrypt_keystore: unsupported cipher: " + cipher);
    }
    if (kdf != "scrypt") {
        throw std::runtime_error("decrypt_keystore: unsupported kdf: " + kdf);
    }

    auto ciphertext = hex_decode(crypto.at("ciphertext").get<std::string>());
    auto iv = hex_decode(crypto.at("cipherparams").at("iv").get<std::string>());
    auto mac_expected = hex_decode(crypto.at("mac").get<std::string>());
    auto& kp = crypto.at("kdfparams");
    auto salt = hex_decode(kp.at("salt").get<std::string>());
    int dklen = kp.at("dklen").get<int>();
    uint64_t N = kp.at("n").get<uint64_t>();
    uint64_t r = kp.at("r").get<uint64_t>();
    uint64_t p = kp.at("p").get<uint64_t>();

    if (ciphertext.size() != 32) throw std::runtime_error("decrypt_keystore: ciphertext must be 32 bytes");
    if (iv.size() != 16) throw std::runtime_error("decrypt_keystore: iv must be 16 bytes");
    if (mac_expected.size() != 32) throw std::runtime_error("decrypt_keystore: mac must be 32 bytes");
    if (dklen != 32) throw std::runtime_error("decrypt_keystore: dklen must be 32");

    std::vector<uint8_t> derived(dklen);
    scrypt_kdf(password, salt.data(), salt.size(), N, r, p, derived.data(), dklen);

    // MAC check (constant-time)
    uint8_t mac_input[16 + 32];
    std::memcpy(mac_input, derived.data() + 16, 16);
    std::memcpy(mac_input + 16, ciphertext.data(), 32);
    auto mac_actual = keccak256(mac_input, sizeof(mac_input));
    if (!constant_time_eq(mac_actual.data(), mac_expected.data(), 32)) {
        secure_zero(derived.data(), derived.size());
        throw std::runtime_error("invalid password (MAC mismatch)");
    }

    // AES-128-CTR decrypt（CTR 加解密同函数，密钥取 derived[0..16]）
    uint8_t plain[32];
    aes_128_ctr(derived.data(), iv.data(), ciphertext.data(), plain, 32);

    PrivateKey key(plain);

    secure_zero(plain, 32);
    secure_zero(derived.data(), derived.size());
    secure_zero(mac_input, sizeof(mac_input));

    return key;
}

}  // namespace polymarket
