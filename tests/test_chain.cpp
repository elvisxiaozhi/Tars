// test_chain —— 直接打 ChainClient，验证 R3 RPC 链路 + ABI encode
//
// 使用：./build/test_chain [proxy_address]
//   不传参数：用 0x528508f3...（用户 EOA，余额预期 = 0）
//   传地址：测查那个地址
//
// 走全套：USDC.e + USDC native 余额、对 CTFExchange 的 allowance

#include <cstdlib>
#include <iostream>
#include <string>

#include "net/chain_client.h"

using namespace polymarket::net;

int main(int argc, char* argv[]) {
    std::string addr = (argc > 1) ? argv[1]
                                  : "0x528508f3b308009644dBD0b57325C368c5c1a8fa";

    std::cout << "Testing ChainClient against address: " << addr << "\n\n";

    std::vector<std::string> rpcs = {
        "https://polygon-bor-rpc.publicnode.com",
        "https://polygon.drpc.org",
        "https://1rpc.io/matic"
    };

    // 走 macOS 本地代理（用户环境）
    std::string proxy = "http://127.0.0.1:7897";
    if (const char* env = std::getenv("HTTP_PROXY")) proxy = env;

    ChainClient chain(rpcs, proxy);

    const std::string USDC_E   = "0x2791Bca1f2de4661ED88A30C99A7a9449Aa84174";
    const std::string USDC_N   = "0x3c499c542cEF5E3811e1192ce70d8cC03d5c3359";
    const std::string EXCHANGE = "0x4bFb41d5B3570DeFd03C39a9A4D8dE6Bd8B8982E";

    auto query_balance = [&](const std::string& token, const std::string& label) {
        try {
            auto data = ChainClient::encode_balance_of(addr);
            auto raw = chain.eth_call(token, data);
            double v = ChainClient::decode_usdc6(raw);
            std::cout << "  " << label << " balance: $" << v
                      << "  (raw=" << raw.substr(0, 18) << "...)\n";
        } catch (const std::exception& e) {
            std::cout << "  " << label << " balance: ERROR " << e.what() << "\n";
        }
    };

    auto query_allowance = [&](const std::string& token, const std::string& label) {
        try {
            auto data = ChainClient::encode_allowance(addr, EXCHANGE);
            auto raw = chain.eth_call(token, data);
            double v = ChainClient::decode_usdc6(raw);
            std::cout << "  " << label << " allowance(→Exchange): $" << v << "\n";
        } catch (const std::exception& e) {
            std::cout << "  " << label << " allowance: ERROR " << e.what() << "\n";
        }
    };

    std::cout << "[ERC-20 balanceOf]\n";
    query_balance(USDC_E, "USDC.e   ");
    query_balance(USDC_N, "USDC.nat ");

    std::cout << "\n[ERC-20 allowance(addr, CTFExchange)]\n";
    query_allowance(USDC_E, "USDC.e   ");
    query_allowance(USDC_N, "USDC.nat ");

    std::cout << "\n[Native balance]\n";
    try {
        auto raw = chain.eth_get_balance(addr);
        // wei → MATIC：18 位精度，借用 decode_uint256 + 字符串处理
        std::cout << "  MATIC raw:  " << raw << "\n";
        std::cout << "  MATIC dec:  " << ChainClient::decode_uint256(raw) << " wei\n";
    } catch (const std::exception& e) {
        std::cout << "  MATIC: ERROR " << e.what() << "\n";
    }

    std::cout << "\n[Decode unit tests]\n";
    // 0x70a08231 + 32 字节 0 → balance 0
    std::cout << "  decode 0x000...000 → " << ChainClient::decode_uint256("0x" + std::string(64, '0')) << " (expect 0)\n";
    // 0x...ff (max uint256) — 超 int64，走慢路径
    std::cout << "  decode 0x100 → " << ChainClient::decode_uint256("0x100") << " (expect 256)\n";
    std::cout << "  decode 1e6 hex (0xf4240) → " << ChainClient::decode_uint256("0xf4240") << " (expect 1000000)\n";
    std::cout << "  decode_usdc6(0xf4240) → " << ChainClient::decode_usdc6("0xf4240") << " (expect 1.0)\n";

    return 0;
}
