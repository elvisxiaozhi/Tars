cmake_minimum_required(VERSION 3.20)
cmake_policy(SET CMP0144 NEW)
cmake_policy(SET CMP0167 NEW)
project(polymarket-arb VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# --- Brew prefix (Apple Silicon) ---
set(HOMEBREW_PREFIX "/opt/homebrew")
list(APPEND CMAKE_PREFIX_PATH "${HOMEBREW_PREFIX}")

# --- Boost ---
find_package(Boost 1.80 REQUIRED)

# --- OpenSSL ---
set(OPENSSL_ROOT_DIR "${HOMEBREW_PREFIX}/opt/openssl@3")
find_package(OpenSSL 3.0 REQUIRED)

# --- SQLite3 (macOS built-in) ---
find_library(SQLITE3_LIB sqlite3)

# --- secp256k1 ---
find_library(SECP256K1_LIB secp256k1 PATHS "${HOMEBREW_PREFIX}/lib")
find_path(SECP256K1_INCLUDE secp256k1.h PATHS "${HOMEBREW_PREFIX}/include")

# --- spdlog (手动, 编译为静态库) ---
add_subdirectory(third_party/spdlog)

# --- nlohmann/json (header-only) ---
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE ${CMAKE_SOURCE_DIR}/third_party)

# --- crypto 静态库（Keccak-256 + Web3 keystore + EIP-712 + L2 HMAC 基建） ---
add_library(polymarket_crypto STATIC
    src/crypto/keccak256.cpp
    src/crypto/wallet.cpp
    src/crypto/eip712.cpp
    src/crypto/base64.cpp
    src/crypto/hmac_sha256.cpp
)
target_include_directories(polymarket_crypto PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${SECP256K1_INCLUDE}
)
target_link_libraries(polymarket_crypto PUBLIC
    OpenSSL::Crypto
    ${SECP256K1_LIB}
    nlohmann_json
)

# --- 主程序源文件 ---
set(SOURCES
    src/main.cpp
    src/net/http_client.cpp
    src/net/ws_client.cpp
    src/utils/config.cpp
    src/core/market_feed.cpp
    src/core/binance_feed.cpp
    src/core/clob_ws_feed.cpp
    src/core/quote_cache.cpp
    src/core/strategy.cpp
    src/core/experiment_engine.cpp
    src/core/risk_manager.cpp
    src/core/trade_journal.cpp
    src/core/live_trader.cpp
    src/core/polymarket_types.cpp
    src/net/api_server.cpp
    src/net/chain_client.cpp
)

add_executable(${PROJECT_NAME} ${SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${SECP256K1_INCLUDE}
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    Boost::boost
    OpenSSL::SSL
    OpenSSL::Crypto
    ${SQLITE3_LIB}
    ${SECP256K1_LIB}
    spdlog::spdlog
    nlohmann_json
    polymarket_crypto
    pthread
)

# --- 测试可执行文件 ---
add_executable(test_crypto
    tests/test_crypto.cpp
    src/core/polymarket_types.cpp  # build_hmac_l2 实现（base64+HMAC 集成测试）
)
target_include_directories(test_crypto PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_crypto PRIVATE polymarket_crypto)

# test_chain: 验证 ChainClient + 真实 Polygon RPC（R3）
add_executable(test_chain
    tests/test_chain.cpp
    src/net/chain_client.cpp
    src/net/http_client.cpp
)
target_include_directories(test_chain PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_chain PRIVATE
    Boost::boost
    OpenSSL::SSL
    OpenSSL::Crypto
    nlohmann_json
    spdlog::spdlog
    pthread
)

# test_eip712: EIP-712 signing KATs (R4)
add_executable(test_eip712
    tests/test_eip712.cpp
    src/core/polymarket_types.cpp
)
target_include_directories(test_eip712 PRIVATE ${CMAKE_SOURCE_DIR}/src ${SECP256K1_INCLUDE})
target_link_libraries(test_eip712 PRIVATE polymarket_crypto)

# --- 一次性 CLI 工具 ---
# import_key: 把明文私钥加密成 keystore.enc（R2b）
add_executable(import_key tools/import_key.cpp)
target_link_libraries(import_key PRIVATE polymarket_crypto)
