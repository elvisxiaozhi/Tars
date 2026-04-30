#include "net/chain_client.h"

#include <atomic>
#include <stdexcept>

#include <json.hpp>
#include <spdlog/spdlog.h>

#include "net/http_client.h"

namespace polymarket::net {

using json = nlohmann::json;

struct ChainClient::Impl {
    std::vector<std::string> rpcs;
    HttpClient http;
    std::atomic<size_t> current{0};  // 当前优先 RPC 索引

    Impl(const std::vector<std::string>& rpc_urls,
         const std::string& proxy, int timeout_sec)
        : rpcs(rpc_urls), http(timeout_sec, proxy) {
        if (rpcs.empty()) throw std::invalid_argument("ChainClient: rpc_urls empty");
    }

    // 发一笔 JSON-RPC，按当前 index 起 failover
    json call(const std::string& method, const json& params) {
        json req = {
            {"jsonrpc", "2.0"},
            {"id", 1},
            {"method", method},
            {"params", params}
        };
        std::string body = req.dump();

        size_t start = current.load();
        std::string last_err;
        for (size_t i = 0; i < rpcs.size(); ++i) {
            size_t idx = (start + i) % rpcs.size();
            const auto& url = rpcs[idx];
            try {
                auto res = http.post(url, body, {{"Content-Type", "application/json"}});
                if (res.status_code != 200) {
                    last_err = "HTTP " + std::to_string(res.status_code) + " from " + url;
                    spdlog::debug("rpc failover: {} → {}", url, res.status_code);
                    continue;
                }
                auto resp = json::parse(res.body);
                if (resp.contains("error")) {
                    last_err = "RPC error from " + url + ": " + resp["error"].dump();
                    spdlog::debug("rpc failover: {} → {}", url, last_err);
                    continue;
                }
                if (!resp.contains("result")) {
                    last_err = "no result field from " + url;
                    continue;
                }
                if (idx != start) {
                    current.store(idx);  // 把工作的 RPC 升为下次首选
                }
                return resp["result"];
            } catch (const std::exception& e) {
                last_err = std::string(e.what()) + " from " + url;
                spdlog::debug("rpc failover: {} → {}", url, e.what());
            }
        }
        throw std::runtime_error("ChainClient: all RPCs failed, last: " + last_err);
    }
};

ChainClient::ChainClient(const std::vector<std::string>& rpc_urls,
                         const std::string& proxy_url,
                         int timeout_sec)
    : impl_(std::make_unique<Impl>(rpc_urls, proxy_url, timeout_sec)) {}

ChainClient::~ChainClient() = default;

std::string ChainClient::eth_call(const std::string& to, const std::string& data) {
    json params = json::array({
        {{"to", to}, {"data", data}},
        "latest"
    });
    auto r = impl_->call("eth_call", params);
    if (!r.is_string()) throw std::runtime_error("eth_call: result not string");
    return r.get<std::string>();
}

std::string ChainClient::eth_get_balance(const std::string& address) {
    json params = json::array({address, "latest"});
    auto r = impl_->call("eth_getBalance", params);
    if (!r.is_string()) throw std::runtime_error("eth_getBalance: result not string");
    return r.get<std::string>();
}

// ============== ABI encoding ==============

namespace {

std::string strip_0x(const std::string& hex) {
    if (hex.size() >= 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) {
        return hex.substr(2);
    }
    return hex;
}

std::string lpad_to(const std::string& s, size_t target, char pad = '0') {
    if (s.size() >= target) return s;
    return std::string(target - s.size(), pad) + s;
}

}  // namespace

std::string ChainClient::abi_encode_address(const std::string& addr) {
    auto h = strip_0x(addr);
    if (h.size() != 40) {
        throw std::invalid_argument("abi_encode_address: expected 20 bytes (40 hex), got " +
                                    std::to_string(h.size()));
    }
    // 转小写以保证一致
    std::string lo;
    lo.reserve(40);
    for (char c : h) {
        if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
        lo.push_back(c);
    }
    return lpad_to(lo, 64);
}

std::string ChainClient::abi_encode_uint256(const std::string& dec_or_hex) {
    auto h = strip_0x(dec_or_hex);
    // 简化：仅支持 hex 形式（caller 自行转换）
    if (h.size() > 64) throw std::invalid_argument("abi_encode_uint256: > 32 bytes");
    return lpad_to(h, 64);
}

std::string ChainClient::encode_balance_of(const std::string& holder) {
    // selector(balanceOf(address)) = 0x70a08231
    return "0x70a08231" + abi_encode_address(holder);
}

std::string ChainClient::encode_allowance(const std::string& owner,
                                          const std::string& spender) {
    // selector(allowance(address,address)) = 0xdd62ed3e
    return "0xdd62ed3e" + abi_encode_address(owner) + abi_encode_address(spender);
}

// ============== Decoding ==============

std::string ChainClient::decode_uint256(const std::string& hex_with_0x) {
    auto h = strip_0x(hex_with_0x);
    if (h.empty()) return "0";
    // 去前导 0
    size_t start = 0;
    while (start < h.size() - 1 && h[start] == '0') ++start;
    h = h.substr(start);

    // hex → 十进制（手动大数除法，h 可达 64 个 hex 字符 = 256 bit）
    // 用字节数组做除法循环，效率不高但可读
    std::vector<uint8_t> nibbles;
    nibbles.reserve(h.size());
    for (char c : h) {
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else throw std::invalid_argument("decode_uint256: non-hex char");
        nibbles.push_back(static_cast<uint8_t>(v));
    }

    std::string out;
    while (!nibbles.empty()) {
        // 整除 10
        std::vector<uint8_t> next;
        next.reserve(nibbles.size());
        unsigned acc = 0;
        for (uint8_t n : nibbles) {
            acc = acc * 16 + n;
            unsigned q = acc / 10;
            acc = acc % 10;
            if (!next.empty() || q != 0) next.push_back(static_cast<uint8_t>(q));
        }
        out.push_back(static_cast<char>('0' + acc));
        nibbles = std::move(next);
    }
    if (out.empty()) return "0";
    std::reverse(out.begin(), out.end());
    return out;
}

double ChainClient::decode_usdc6(const std::string& hex_with_0x) {
    auto dec = decode_uint256(hex_with_0x);
    // dec 是 6 位精度整数。直接用 strtoll 然后除 1e6
    // 注意：超过 9.2e12 USDC 会溢出 int64。Polymarket 用户余额远不会到。
    try {
        long long v = std::stoll(dec);
        return static_cast<double>(v) / 1e6;
    } catch (...) {
        // 超大数：手动取前 13 位再算
        if (dec.size() > 13) {
            long long head = std::stoll(dec.substr(0, dec.size() - 6));
            long long frac = std::stoll(dec.substr(dec.size() - 6));
            return static_cast<double>(head) + static_cast<double>(frac) / 1e6;
        }
        return 0.0;
    }
}

}  // namespace polymarket::net
