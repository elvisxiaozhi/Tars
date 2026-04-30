#pragma once

// ChainClient —— Polygon JSON-RPC 客户端 + ABI encode 辅助
//
// 设计：
//   - 走 net::HttpClient::post（已支持 HTTPS + 代理）
//   - 多 RPC failover：调用失败/限速时按列表顺序切换下一个
//   - 仅实现 R3 需要的 read-only 方法（eth_call / eth_getBalance）
//   - 写交易（eth_sendRawTransaction）留到 R7+

#include <memory>
#include <string>
#include <vector>

namespace polymarket::net {

class HttpClient;  // forward decl

class ChainClient {
public:
    // rpc_urls 必须非空；调用时按顺序尝试，遇 4xx/5xx/timeout 切下一个
    ChainClient(const std::vector<std::string>& rpc_urls,
                const std::string& proxy_url = "",
                int timeout_sec = 15);
    ~ChainClient();

    ChainClient(const ChainClient&) = delete;
    ChainClient& operator=(const ChainClient&) = delete;

    // ===== JSON-RPC primitives =====

    // eth_call: 只读调用合约（latest 块）。
    // to: 0x... (20 字节合约地址)；data: 0x... (calldata，先 selector 后参数)
    // 返回：return data hex（带 0x 前缀）
    std::string eth_call(const std::string& to, const std::string& data);

    // eth_getBalance: 原生代币（MATIC/POL）余额，返回 wei（uint256 大数；string）
    std::string eth_get_balance(const std::string& address);

    // ===== ABI encode helpers =====

    // ERC-20 / ERC-1155 等单地址查询：32 字节左 pad
    static std::string abi_encode_address(const std::string& addr);

    // uint256 → 32 字节左 pad hex
    static std::string abi_encode_uint256(const std::string& dec_or_hex);

    // 取 ERC-20 balanceOf(address) 的 calldata
    static std::string encode_balance_of(const std::string& holder);

    // 取 ERC-20 allowance(owner, spender) 的 calldata
    static std::string encode_allowance(const std::string& owner,
                                        const std::string& spender);

    // ===== 解码：32 字节 hex → 十进制 uint256 字符串 =====
    // 返回 string 因为 USDC 6 位精度时数值可能 > uint64 上限
    static std::string decode_uint256(const std::string& hex_with_0x);

    // 32 字节 hex → 6 位精度的 USDC 浮点（仅适用 ≤ ~9e12 的金额，超过会损精度）
    static double decode_usdc6(const std::string& hex_with_0x);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace polymarket::net
