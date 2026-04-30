#include "core/live_trader.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include <spdlog/spdlog.h>

#include "crypto/wallet.h"
#include "net/chain_client.h"

namespace polymarket {

LiveTrader::LiveTrader(const AppConfig& cfg) : cfg_(cfg) {}
LiveTrader::~LiveTrader() = default;

// ===== 钱包（R2）=====
//
// 流程：
//   1. 读 keystore.enc 文件（路径来自 cfg_.wallet.keystore_path）
//   2. 拿密码：优先 KEYSTORE_PASSWORD env var；否则 stdin 提示输入
//   3. decrypt_keystore → derive_address
//   4. 与 cfg_.wallet.address 比对（case-insensitive），不一致 throw
//   5. 缓存地址到 address_ + 置 wallet_initialized_=true
//
// 异常会一路抛到 main()，由启动守卫捕获并 return 1
void LiveTrader::init_wallet() {
    if (wallet_initialized_) return;

    const std::string& path = cfg_.wallet.keystore_path;
    if (path.empty()) {
        throw std::runtime_error("init_wallet: cfg.wallet.keystore_path is empty");
    }

    std::ifstream f(path);
    if (!f) {
        throw std::runtime_error("init_wallet: cannot open keystore file: " + path);
    }
    std::stringstream ss;
    ss << f.rdbuf();
    std::string keystore_json = ss.str();
    if (keystore_json.empty()) {
        throw std::runtime_error("init_wallet: keystore file is empty: " + path);
    }

    // 密码来源：env var 优先（自动化场景），否则交互式
    std::string password;
    const char* env_pwd = std::getenv("KEYSTORE_PASSWORD");
    if (env_pwd && *env_pwd) {
        password = env_pwd;
        spdlog::info("init_wallet: using password from KEYSTORE_PASSWORD env var");
    } else {
        password = read_password(
            "Enter keystore password for " + path + " (no echo): ");
    }

    if (password.empty()) {
        throw std::runtime_error("init_wallet: empty password");
    }

    PrivateKey key;
    try {
        key = decrypt_keystore(keystore_json, password);
    } catch (...) {
        secure_zero(password.data(), password.size());
        throw;
    }
    secure_zero(password.data(), password.size());

    std::string derived = derive_address(key);

    // 比对 cfg.wallet.address（case-insensitive，因为 EIP-55 大小写易出错）
    if (!cfg_.wallet.address.empty()) {
        bool match = (derived.size() == cfg_.wallet.address.size());
        if (match) {
            for (size_t i = 0; i < derived.size() && match; ++i) {
                char a = derived[i], b = cfg_.wallet.address[i];
                if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
                if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
                if (a != b) match = false;
            }
        }
        if (!match) {
            throw std::runtime_error(
                "init_wallet: derived address does NOT match cfg.wallet.address (expected " +
                cfg_.wallet.address + ", got " + derived + ")");
        }
    }

    address_ = derived;
    wallet_initialized_ = true;
    spdlog::info("init_wallet: OK, address={}", address_);
}

std::string LiveTrader::wallet_address() const {
    if (!wallet_initialized_) {
        throw std::runtime_error("LiveTrader::wallet_address called before init_wallet");
    }
    return address_;
}

// ===== 链上读（R3）=====
//
// 查询目标 = polymarket.proxy_address（持有 USDC + CTF 的代理钱包）
// 一次性发出 4 个 eth_call：
//   1. USDC.e .balanceOf(proxy)
//   2. USDC native .balanceOf(proxy)
//   3. USDC.e .allowance(proxy, CTFExchange)
//   4. USDC native .allowance(proxy, CTFExchange)
//
// CTF 余额暂不查（需要先知道 token_ids；token_ids 由 strategy 在持仓时给出，
// 当前空仓状态下没有查询对象）。R7+ 在下单后会用 ChainBalance.ctf_balances 字段。
ChainBalance LiveTrader::read_chain_state(const std::vector<std::string>& ctf_token_ids) {
    if (cfg_.polymarket.proxy_address.empty()) {
        throw std::runtime_error(
            "read_chain_state: polymarket.proxy_address is empty (live 模式必填)");
    }
    if (cfg_.polygon.rpc_urls.empty()) {
        throw std::runtime_error("read_chain_state: polygon.rpc_urls is empty");
    }

    net::ChainClient chain(cfg_.polygon.rpc_urls, cfg_.network.proxy_url);

    const auto& proxy = cfg_.polymarket.proxy_address;
    const auto& exchange = cfg_.polygon.ctf_exchange;

    ChainBalance bal;
    bal.snapshot_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // USDC.e + USDC native 余额
    double usdc_e = net::ChainClient::decode_usdc6(
        chain.eth_call(cfg_.polygon.usdc_e_address,
                       net::ChainClient::encode_balance_of(proxy)));
    double usdc_native = net::ChainClient::decode_usdc6(
        chain.eth_call(cfg_.polygon.usdc_native_address,
                       net::ChainClient::encode_balance_of(proxy)));
    bal.usdc = usdc_e + usdc_native;

    // Allowance：proxy → CTFExchange
    double allowance_e = net::ChainClient::decode_usdc6(
        chain.eth_call(cfg_.polygon.usdc_e_address,
                       net::ChainClient::encode_allowance(proxy, exchange)));
    double allowance_native = net::ChainClient::decode_usdc6(
        chain.eth_call(cfg_.polygon.usdc_native_address,
                       net::ChainClient::encode_allowance(proxy, exchange)));
    bal.allowance_usdc = std::max(allowance_e, allowance_native);

    spdlog::info("CHAIN: proxy={} | USDC.e=${:.4f} USDC.native=${:.4f} | "
                 "allowance.e=${:.2f} allowance.native=${:.2f}",
                 proxy, usdc_e, usdc_native, allowance_e, allowance_native);

    if (!ctf_token_ids.empty()) {
        spdlog::warn("read_chain_state: CTF balanceOfBatch not implemented yet (R7+)");
    }

    return bal;
}

// ===== CLOB 鉴权（R5）=====
void LiveTrader::ensure_clob_authenticated() {
    throw std::runtime_error("LiveTrader::ensure_clob_authenticated not implemented (R5 pending)");
}

bool LiveTrader::is_authenticated() const {
    return clob_authenticated_;
}

// ===== Approval 校验（R6）=====
bool LiveTrader::check_approvals_sufficient(double) {
    throw std::runtime_error("LiveTrader::check_approvals_sufficient not implemented (R6 pending)");
}

// ===== 下单（R7 / R8）=====
OrderResult LiveTrader::place_entry_order(const EntrySignal&, double) {
    throw std::runtime_error("LiveTrader::place_entry_order not implemented (R7 pending)");
}

OrderResult LiveTrader::place_exit_order(const Position&, double, bool, const std::string&) {
    throw std::runtime_error("LiveTrader::place_exit_order not implemented (R8 pending)");
}

// ===== 启动对账 + 应急平仓（R9）=====
void LiveTrader::reconcile_on_startup() {
    throw std::runtime_error("LiveTrader::reconcile_on_startup not implemented (R9 pending)");
}

void LiveTrader::emergency_close_all(const std::vector<Position>&) {
    throw std::runtime_error("LiveTrader::emergency_close_all not implemented (R9 pending)");
}

}  // namespace polymarket
