#include "core/live_trader.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <random>
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

    // 更新缓存
    cached_cash_pusd_        = bal.cash_pusd;
    last_balance_refresh_ms_ = bal.snapshot_ms;

    spdlog::info("POLY: cash=${:.4f} pUSD  allowance=${:.4f}  (V2 vault ledger)",
                 bal.cash_pusd, bal.allowance);

    return bal;
}

// 节流刷新：如果上次刷新超过 max_age_ms 就调一次 read_polymarket_balance；否则 no-op。
// 调用方可每 tick 调，自动节流避免 rate limit。
void LiveTrader::refresh_balance_if_stale(int64_t max_age_ms) {
    if (!clob_authenticated_) return;
    int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (last_balance_refresh_ms_ != 0 && (now - last_balance_refresh_ms_) < max_age_ms)
        return;
    try {
        read_polymarket_balance();  // 内部更新 cache
    } catch (const std::exception& e) {
        spdlog::warn("refresh_balance_if_stale failed: {}", e.what());
    }
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

// ===== 查单笔订单（R-V2.7-P0）=====
// 与 SDK py-clob-client-v2 client.py:get_order 行为对齐：
//   path = /data/order/<order_id>（含 id），L2 HMAC 签同样 path
//   响应字段：id / status / size_matched / ...
OrderDetail LiveTrader::get_order(const std::string& order_id) {
    OrderDetail d;
    d.order_id = order_id;

    if (!clob_authenticated_) { d.error = "not authenticated"; return d; }
    if (order_id.empty())     { d.error = "empty order_id";    return d; }

    std::string path = "/data/order/" + order_id;

    uint64_t ts        = static_cast<uint64_t>(std::time(nullptr));
    std::string ts_str = std::to_string(ts);
    std::string sig_b64 = build_hmac_l2(clob_secret_, ts_str, "GET", path);

    net::Headers headers = {
        {"POLY_ADDRESS",    address_},
        {"POLY_SIGNATURE",  sig_b64},
        {"POLY_TIMESTAMP",  ts_str},
        {"POLY_API_KEY",    clob_api_key_},
        {"POLY_PASSPHRASE", clob_passphrase_},
    };

    net::HttpClient http(15, cfg_.network.proxy_url);
    auto resp = http.get(cfg_.polymarket.clob_rest_url + path, headers);

    if (resp.status_code != 200) {
        d.error = "HTTP " + std::to_string(resp.status_code) + " body=" + resp.body;
        return d;
    }

    try {
        auto j = nlohmann::json::parse(resp.body);
        d.status = j.value("status", std::string{});
        // 服务端可能返回大写（MATCHED）或小写（matched）— 统一转小写便于调用方比较
        for (auto& c : d.status) c = static_cast<char>(std::tolower(c));
        // size_matched 兼容 string/number
        if (j.contains("size_matched")) {
            const auto& v = j.at("size_matched");
            if (v.is_string()) d.size_matched = std::stod(v.get<std::string>());
            else if (v.is_number()) d.size_matched = v.get<double>();
        }
        d.ok = true;
    } catch (const std::exception& e) {
        d.error = std::string("parse: ") + e.what();
    }
    return d;
}

// ===== 取消订单（R-V2.5c）=====
//
// 与 SDK py-clob-client-v2 client.py:cancel_order 行为对齐：
//   DELETE /order  body = {"orderID":"<id>"}  L2 HMAC（method=DELETE, path=/order）
bool LiveTrader::cancel_order(const std::string& order_id) {
    if (!clob_authenticated_)
        throw std::runtime_error("cancel_order: ensure_clob_authenticated first");
    if (order_id.empty())
        throw std::runtime_error("cancel_order: order_id empty");

    std::string body = "{\"orderID\":\"" + order_id + "\"}";

    uint64_t ts        = static_cast<uint64_t>(std::time(nullptr));
    std::string ts_str = std::to_string(ts);
    std::string sig_b64 = build_hmac_l2(clob_secret_, ts_str, "DELETE", "/order", body);

    net::Headers headers = {
        {"POLY_ADDRESS",    address_},
        {"POLY_SIGNATURE",  sig_b64},
        {"POLY_TIMESTAMP",  ts_str},
        {"POLY_API_KEY",    clob_api_key_},
        {"POLY_PASSPHRASE", clob_passphrase_},
        {"Content-Type",    "application/json"},
    };

    net::HttpClient http(15, cfg_.network.proxy_url);
    std::string url = cfg_.polymarket.clob_rest_url + "/order";
    auto resp = http.del(url, body, headers);

    if (resp.status_code != 200) {
        spdlog::error("DELETE /order failed: HTTP {} body={}", resp.status_code, resp.body);
        return false;
    }

    spdlog::info("DELETE /order 200: {}", resp.body);
    return true;
}

// ===== 下单（R-V2.5b dry）=====
//
// R-V2.5b-dry：只构造 + 签 + 序列化，**不发 POST**，把 body 打到日志让用户肉眼验。
// R-V2.5b-live（下一步）会去掉 dry 守卫，真发 POST /order 并解析 OrderResult。

namespace {

// salt：与 py-clob-client-v2 行为接近 — 60-bit 范围内随机正整数（不强制完整 uint256）
uint64_t generate_salt() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    return std::uniform_int_distribution<uint64_t>(1, (1ULL << 60) - 1)(rng);
}

// 当前 unix 毫秒
uint64_t now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

// 6-decimal micro-units：value × 1e6 四舍五入
uint64_t to_micro(double v) {
    return static_cast<uint64_t>(std::llround(v * 1'000'000.0));
}

}  // namespace

// 通用 V2 下单 helper —— BUY 和 SELL 共享。差异：
//   BUY:  side=0, makerAmount = shares × price（出 pUSD）, takerAmount = shares
//   SELL: side=1, makerAmount = shares（出 shares）,        takerAmount = shares × price
// is_taker=true 时未改 orderType（仍 GTC）；FOK 路径留给后续根据 server 行为再加。
OrderResult LiveTrader::send_v2_order(
    const std::string& token_id, double price, double shares,
    bool is_buy, bool is_taker, const std::string& tag)
{
    if (!clob_authenticated_)
        throw std::runtime_error("send_v2_order: ensure_clob_authenticated first");
    if (token_id.empty())
        throw std::runtime_error("send_v2_order: token_id empty");
    if (shares <= 0)
        throw std::runtime_error("send_v2_order: shares must be > 0");
    if (price <= 0 || price >= 1)
        throw std::runtime_error("send_v2_order: price must be in (0, 1)");

    PolyOrder order{};
    order.salt           = generate_salt();
    order.maker          = cfg_.polymarket.api_address;  // V2: api_address (不是 proxy_address)
    order.signer         = address_;
    order.token_id       = token_id;
    if (is_buy) {
        order.maker_amount = to_micro(shares * price);  // 出 pUSD
        order.taker_amount = to_micro(shares);          // 收 shares
        order.side         = 0;
    } else {
        order.maker_amount = to_micro(shares);          // 出 shares
        order.taker_amount = to_micro(shares * price);  // 收 pUSD
        order.side         = 1;
    }
    order.signature_type = 1;   // POLY_PROXY
    order.timestamp      = now_ms();

    Signature s = sign_poly_order(key_, order,
                                  static_cast<uint64_t>(cfg_.polymarket.chain_id));
    std::string order_json = polyorder_to_json(order, s);

    // FOK = 即时成交否则取消（用于 market 出场 / 止损）；GTC = 挂单等吃
    const char* order_type = is_taker ? "FOK" : "GTC";

    std::string body =
        "{\"order\":" + order_json +
        ",\"owner\":\"" + clob_api_key_ + "\""
        ",\"orderType\":\"" + order_type + "\""
        ",\"deferExec\":false"
        ",\"postOnly\":false}";

    spdlog::info("POST /order  [{}] {} shares={:.4f} price={:.4f} type={} token={}...",
                 tag, is_buy ? "BUY" : "SELL", shares, price, order_type,
                 token_id.substr(0, 16));
    spdlog::debug("  body: {}", body);

    uint64_t ts        = static_cast<uint64_t>(std::time(nullptr));
    std::string ts_str = std::to_string(ts);
    std::string sig_b64 = build_hmac_l2(clob_secret_, ts_str, "POST", "/order", body);

    net::Headers headers = {
        {"POLY_ADDRESS",    address_},
        {"POLY_SIGNATURE",  sig_b64},
        {"POLY_TIMESTAMP",  ts_str},
        {"POLY_API_KEY",    clob_api_key_},
        {"POLY_PASSPHRASE", clob_passphrase_},
        {"Content-Type",    "application/json"},
    };

    net::HttpClient http(15, cfg_.network.proxy_url);
    std::string url = cfg_.polymarket.clob_rest_url + "/order";
    auto resp = http.post(url, body, headers);

    OrderResult result;
    result.fill_time_ms = static_cast<int64_t>(order.timestamp);

    if (resp.status_code != 200) {
        result.success = false;
        result.error   = "HTTP " + std::to_string(resp.status_code) + " body=" + resp.body;
        spdlog::error("POST /order [{}] failed: {}", tag, result.error);
        return result;
    }

    spdlog::info("POST /order [{}] 200: {}", tag, resp.body);

    try {
        auto j = nlohmann::json::parse(resp.body);
        result.success  = j.value("success", false);
        result.order_id = j.value("orderID", j.value("orderId", std::string{}));
        if (!result.success) {
            result.error = j.value("errorMsg", j.dump());
        }
    } catch (const std::exception& e) {
        result.success = false;
        result.error   = std::string("response parse failed: ") + e.what();
    }

    return result;
}

OrderResult LiveTrader::place_entry_order(const EntrySignal& sig, double shares) {
    return send_v2_order(sig.token_id, sig.entry_price, shares,
                         /*is_buy=*/true, /*is_taker=*/false, "entry");
}

OrderResult LiveTrader::place_exit_order(const Position& pos, double shares,
                                         double price, bool is_taker,
                                         const std::string& reason) {
    return send_v2_order(pos.token_id, price, shares,
                         /*is_buy=*/false, is_taker, "exit:" + reason);
}

// ===== 启动对账 + 应急平仓（R9-V2）=====
//
// reconcile_on_startup：GET /data/orders（L2 HMAC）+ 分页 cursor 读完所有 open orders。
// 与 SDK get_open_orders 行为一致：first cursor "MA==" (base64 "0")；终止 "LTE=" (base64 "-1")。
// HMAC 只签 path "/data/orders"，query (next_cursor=...) 单独传 URL（不参与 HMAC）。
std::vector<OpenOrder> LiveTrader::reconcile_on_startup() {
    if (!clob_authenticated_)
        throw std::runtime_error("reconcile_on_startup: ensure_clob_authenticated first");

    static const char* INITIAL_CURSOR = "MA==";
    static const char* END_CURSOR     = "LTE=";

    std::vector<OpenOrder> orders;
    std::string cursor = INITIAL_CURSOR;

    net::HttpClient http(15, cfg_.network.proxy_url);

    int page = 0;
    while (cursor != END_CURSOR && page < 50 /*safety cap*/) {
        ++page;

        uint64_t ts        = static_cast<uint64_t>(std::time(nullptr));
        std::string ts_str = std::to_string(ts);
        std::string sig_b64 = build_hmac_l2(clob_secret_, ts_str, "GET", "/data/orders");

        net::Headers headers = {
            {"POLY_ADDRESS",    address_},
            {"POLY_SIGNATURE",  sig_b64},
            {"POLY_TIMESTAMP",  ts_str},
            {"POLY_API_KEY",    clob_api_key_},
            {"POLY_PASSPHRASE", clob_passphrase_},
        };

        std::string url = cfg_.polymarket.clob_rest_url + "/data/orders?next_cursor=" + cursor;
        auto resp = http.get(url, headers);
        if (resp.status_code != 200) {
            throw std::runtime_error(
                "reconcile_on_startup: HTTP " + std::to_string(resp.status_code) +
                " body=" + resp.body);
        }

        auto j = nlohmann::json::parse(resp.body);

        // 首次拿到 raw body 用于诊断 / 字段名校准
        if (page == 1) spdlog::debug("RECONCILE raw[1]: {}", resp.body);

        cursor = j.value("next_cursor", END_CURSOR);  // 没字段当作结束

        for (auto& it : j.value("data", nlohmann::json::array())) {
            OpenOrder o;
            o.order_id = it.value("id", "");
            o.token_id = it.value("asset_id", "");
            o.side     = it.value("side", "");
            // 数值字段可能是 string 或 number，做兼容
            auto to_d = [&](const nlohmann::json& v) -> double {
                if (v.is_string()) {
                    const auto& s = v.get_ref<const std::string&>();
                    return s.empty() ? 0.0 : std::stod(s);
                }
                return v.is_number() ? v.get<double>() : 0.0;
            };
            o.price        = to_d(it.value("price", nlohmann::json("0")));
            o.size         = to_d(it.value("original_size", it.value("size", nlohmann::json("0"))));
            o.size_matched = to_d(it.value("size_matched", nlohmann::json("0")));
            o.status       = it.value("status", "");
            orders.push_back(std::move(o));
        }
    }

    spdlog::info("RECONCILE: {} open order(s)", orders.size());
    for (auto& o : orders) {
        spdlog::info("  - id={}... {} {:.4f}@{:.4f} matched={:.4f} status={}",
                     o.order_id.substr(0, std::min<size_t>(o.order_id.size(), 16)),
                     o.side, o.size, o.price, o.size_matched, o.status);
    }

    return orders;
}

// emergency_close_all：两阶段紧急退出。
//   Step 1: 取消所有 open orders（防止 BUY 已发未 fill 时 emergency 触发，
//           错误地走 SELL 路径；先 cancel 才是正确语义）
//   Step 2: 对每个 bot 跟踪的持仓发 SELL FOK @ price=0.01 (floor)，
//           server 按对手最佳 bid 立即成交，否则取消。**不**等 fill 确认。
void LiveTrader::emergency_close_all(const std::vector<Position>& positions) {
    if (!clob_authenticated_) {
        spdlog::error("EMERGENCY: not authenticated, can't close");
        return;
    }

    spdlog::warn("===== EMERGENCY CLOSE ALL =====");

    // Step 1: cancel all open orders
    try {
        auto open = reconcile_on_startup();
        if (open.empty()) {
            spdlog::warn("  [step1] no open orders to cancel");
        } else {
            spdlog::warn("  [step1] cancelling {} open order(s)", open.size());
            for (auto& o : open) {
                bool ok = cancel_order(o.order_id);
                spdlog::warn("    {} {}",
                             o.order_id.substr(0, std::min<size_t>(o.order_id.size(), 16)),
                             ok ? "✅ cancelled" : "⚠️ FAILED");
            }
        }
    } catch (const std::exception& e) {
        spdlog::error("  [step1] reconcile/cancel failed: {}", e.what());
    }

    // Step 2: SELL all bot-tracked positions FOK
    int sent = 0;
    spdlog::warn("  [step2] selling {} position(s) FOK", positions.size());
    for (auto& pos : positions) {
        if (pos.shares <= 0 || pos.closed) continue;
        try {
            auto r = place_exit_order(pos, pos.shares, /*price=*/0.01,
                                      /*is_taker=*/true, "emergency");
            if (r.success)
                spdlog::warn("    SENT  {} shares={:.4f} order_id={}",
                             pos.token_id.substr(0, 16), pos.shares,
                             r.order_id.substr(0, std::min<size_t>(r.order_id.size(), 16)));
            else
                spdlog::error("    FAIL  {} {}", pos.token_id.substr(0, 16), r.error);
            ++sent;
        } catch (const std::exception& e) {
            spdlog::error("    THREW {} {}", pos.token_id.substr(0, 16), e.what());
        }
    }
    spdlog::warn("===== EMERGENCY done: {} sell(s) sent =====", sent);
}

}  // namespace polymarket
