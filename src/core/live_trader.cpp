#include "core/live_trader.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include <json.hpp>
#include <spdlog/spdlog.h>

#include "core/polymarket_types.h"
#include "crypto/eip712.h"
#include "crypto/wallet.h"
#include "net/chain_client.h"
#include "net/http_client.h"

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
    key_ = std::move(key);   // keep for R5/R7/R8 signing
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
//
// 流程：
//   1. 用当前时间戳 + nonce=0 构造 ClobAuthData，EIP-712 签名
//   2. GET /auth/api-key（derive 已有 key）；404 时 POST（create new key）
//   3. 解析 apiKey / secret / passphrase，存成员变量供 R7/R8 使用
//
// 幂等：已鉴权则直接返回（每次启动只调一次）
void LiveTrader::ensure_clob_authenticated() {
    if (clob_authenticated_) return;
    if (!wallet_initialized_)
        throw std::runtime_error("ensure_clob_authenticated: call init_wallet first");

    uint64_t ts = static_cast<uint64_t>(std::time(nullptr));

    ClobAuthData auth;
    auth.address   = address_;
    auth.timestamp = ts;
    auth.nonce     = 0;
    auth.message   = CLOB_AUTH_MESSAGE;

    Signature sig = sign_clob_auth(key_, auth,
                                   static_cast<uint64_t>(cfg_.polymarket.chain_id));

    net::Headers headers = {
        {"POLY_ADDRESS",   address_},
        {"POLY_SIGNATURE", sig.to_hex()},
        {"POLY_TIMESTAMP", std::to_string(ts)},
        {"POLY_NONCE",     "0"},
    };

    net::HttpClient http(15, cfg_.network.proxy_url);

    // create_or_derive：POST /auth/api-key 创建新 key；
    // 若已存在 (HTTP 400 "Could not create api key") 则 GET /auth/derive-api-key 派生
    std::string create_url = cfg_.polymarket.clob_rest_url + "/auth/api-key";
    auto resp = http.post(create_url, "", headers);

    if (resp.status_code != 200) {
        spdlog::info("CLOB: create returned HTTP {} ({}), falling back to derive",
                     resp.status_code, resp.body);
        std::string derive_url = cfg_.polymarket.clob_rest_url + "/auth/derive-api-key";
        resp = http.get(derive_url, headers);
    }

    if (resp.status_code != 200) {
        throw std::runtime_error(
            "ensure_clob_authenticated: CLOB HTTP " +
            std::to_string(resp.status_code) + " body=" + resp.body);
    }

    auto j = nlohmann::json::parse(resp.body);
    clob_api_key_    = j.at("apiKey").get<std::string>();
    clob_secret_     = j.at("secret").get<std::string>();
    clob_passphrase_ = j.at("passphrase").get<std::string>();

    clob_authenticated_ = true;
    spdlog::info("CLOB: authenticated OK, api_key={}...", clob_api_key_.substr(0, 8));
}

bool LiveTrader::is_authenticated() const {
    return clob_authenticated_;
}

// ===== Polymarket V2 cash balance（R-V2.3）=====
//
// 流程：
//   1. 用 R5 拿到的 api_secret 算 L2 HMAC 签名
//   2. GET /balance-allowance?asset_type=COLLATERAL
//   3. 解析 JSON {balance, allowance}（pUSD 6 decimals 字符串）
//
// 备注：V2 升级（2026-04-28）后用户资金不在链上 proxy 钱包，而是 vault ledger 数额，
// 这是唯一能拿到 cash 余额的接口。R3 的 read_chain_state 仅作链上诊断。
PolymarketBalance LiveTrader::read_polymarket_balance() {
    if (!clob_authenticated_)
        throw std::runtime_error("read_polymarket_balance: ensure_clob_authenticated first");

    uint64_t ts        = static_cast<uint64_t>(std::time(nullptr));
    std::string ts_str = std::to_string(ts);
    std::string method = "GET";

    // 关键：HMAC 签的 path **不带 query string**（py-clob-client-v2 行为）；
    // query 走 URL 单独拼接，不参与签名
    std::string path_for_sig = "/balance-allowance";
    std::string query        = "?asset_type=COLLATERAL&signature_type=1";  // POLY_PROXY

    std::string sig = build_hmac_l2(clob_secret_, ts_str, method, path_for_sig);

    net::Headers headers = {
        {"POLY_ADDRESS",    address_},
        {"POLY_SIGNATURE",  sig},
        {"POLY_TIMESTAMP",  ts_str},
        {"POLY_API_KEY",    clob_api_key_},
        {"POLY_PASSPHRASE", clob_passphrase_},
    };

    net::HttpClient http(15, cfg_.network.proxy_url);
    std::string url = cfg_.polymarket.clob_rest_url + path_for_sig + query;
    auto resp = http.get(url, headers);

    if (resp.status_code != 200) {
        throw std::runtime_error(
            "read_polymarket_balance: HTTP " +
            std::to_string(resp.status_code) + " body=" + resp.body);
    }

    spdlog::debug("POLY balance raw: {}", resp.body);

    auto j = nlohmann::json::parse(resp.body);

    // 字段名容错：V1/V2 可能是 string 也可能是 number；6 decimals 单位
    auto parse_amount = [&](const char* key) -> double {
        if (!j.contains(key)) return 0.0;
        const auto& v = j.at(key);
        if (v.is_string()) {
            const auto& s = v.get_ref<const std::string&>();
            if (s.empty()) return 0.0;
            // 大整数字符串，6 decimals → double
            return std::stod(s) / 1e6;
        }
        if (v.is_number()) return v.get<double>() / 1e6;
        return 0.0;
    };

    PolymarketBalance bal;
    bal.cash_pusd  = parse_amount("balance");
    bal.allowance  = parse_amount("allowance");
    bal.snapshot_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    spdlog::info("POLY: cash=${:.4f} pUSD  allowance=${:.4f}  (V2 vault ledger)",
                 bal.cash_pusd, bal.allowance);

    return bal;
}

// ===== Approval 校验（R6）=====
//
// 读链上最新 allowance（proxy → CTFExchange），与 min_usdc_allowance 比较。
// 不足则 throw（启动守卫会捕获并 return 1）；足够则 return true。
// bot 不自动 approve：approve 是 chain write，风险高，由用户手动在 Polymarket 网页完成。
bool LiveTrader::check_approvals_sufficient(double min_usdc_allowance) {
    auto bal = read_chain_state();

    if (bal.allowance_usdc >= min_usdc_allowance) {
        spdlog::info("APPROVAL: allowance ${:.2f} >= required ${:.2f} OK",
                     bal.allowance_usdc, min_usdc_allowance);
        return true;
    }

    // 打印操作指引，然后 throw 让守卫 return 1
    spdlog::error("APPROVAL: allowance ${:.4f} < required ${:.2f}",
                  bal.allowance_usdc, min_usdc_allowance);
    spdlog::error("  proxy address  : {}", cfg_.polymarket.proxy_address);
    spdlog::error("  CTFExchange    : {}", cfg_.polygon.ctf_exchange);
    spdlog::error("  操作方式（任选一）：");
    spdlog::error("    1. Polymarket 网页 → Deposit 充 USDC（充值同时自动 approve）");
    spdlog::error("    2. MetaMask → 手动调用 USDC.approve(CTFExchange, MAX_UINT)");

    throw std::runtime_error(
        "USDC allowance $" + std::to_string(bal.allowance_usdc) +
        " < required $" + std::to_string(min_usdc_allowance) +
        " (proxy=" + cfg_.polymarket.proxy_address + ")");
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
