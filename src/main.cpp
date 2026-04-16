#include <cstdio>

#include <boost/version.hpp>
#include <openssl/opensslv.h>
#include <sqlite3.h>
#include <secp256k1.h>
#include <spdlog/spdlog.h>
#include <json.hpp>

int main() {
    spdlog::info("polymarket-arb v0.1.0");
    spdlog::info("boost     {}.{}.{}",
                 BOOST_VERSION / 100000,
                 BOOST_VERSION / 100 % 1000,
                 BOOST_VERSION % 100);
    spdlog::info("openssl   {}", OPENSSL_VERSION_TEXT);
    spdlog::info("sqlite    {}", sqlite3_libversion());
    spdlog::info("nlohmann  {}.{}.{}",
                 NLOHMANN_JSON_VERSION_MAJOR,
                 NLOHMANN_JSON_VERSION_MINOR,
                 NLOHMANN_JSON_VERSION_PATCH);

    // secp256k1 context 验证
    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    if (ctx) {
        spdlog::info("secp256k1 OK");
        secp256k1_context_destroy(ctx);
    }

    spdlog::info("all dependencies verified");
    return 0;
}
