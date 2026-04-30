#include "crypto/hmac_sha256.h"

#include <stdexcept>

#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/params.h>

namespace polymarket {

std::array<uint8_t, 32> hmac_sha256(const uint8_t* key, size_t key_len,
                                    const uint8_t* msg, size_t msg_len) {
    EVP_MAC* mac = EVP_MAC_fetch(nullptr, "HMAC", nullptr);
    if (!mac) throw std::runtime_error("hmac_sha256: EVP_MAC_fetch failed");

    EVP_MAC_CTX* ctx = EVP_MAC_CTX_new(mac);
    if (!ctx) {
        EVP_MAC_free(mac);
        throw std::runtime_error("hmac_sha256: EVP_MAC_CTX_new failed");
    }

    char digest_name[] = "SHA256";
    OSSL_PARAM params[2] = {
        OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, digest_name, 0),
        OSSL_PARAM_construct_end()
    };

    std::array<uint8_t, 32> out{};
    size_t out_len = out.size();

    bool ok =
        EVP_MAC_init(ctx, key, key_len, params) &&
        EVP_MAC_update(ctx, msg, msg_len) &&
        EVP_MAC_final(ctx, out.data(), &out_len, out.size()) &&
        out_len == 32;

    EVP_MAC_CTX_free(ctx);
    EVP_MAC_free(mac);

    if (!ok) throw std::runtime_error("hmac_sha256: OpenSSL HMAC computation failed");
    return out;
}

}  // namespace polymarket
