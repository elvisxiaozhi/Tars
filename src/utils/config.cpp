#include "utils/config.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <cctype>
#include <functional>
#include <iomanip>
#include <sstream>

#include "core/build_info.h"

#include <json.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>

namespace polymarket {

using json = nlohmann::json;

static std::string jstr(const json& j, const std::string& key, const std::string& def = "") {
    if (j.contains(key) && j[key].is_string()) return j[key].get<std::string>();
    return def;
}

static double jdbl(const json& j, const std::string& key, double def = 0.0) {
    if (j.contains(key) && j[key].is_number()) return j[key].get<double>();
    return def;
}

static int jint(const json& j, const std::string& key, int def = 0) {
    if (j.contains(key) && j[key].is_number_integer()) return j[key].get<int>();
    return def;
}

static bool jbool(const json& j, const std::string& key, bool def = false) {
    if (j.contains(key) && j[key].is_boolean()) return j[key].get<bool>();
    return def;
}

static std::string upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

CoinStrategyConfig default_coin_config(const std::string& coin_in) {
    std::string coin = upper(coin_in);
    CoinStrategyConfig c;
    c.coin = coin;
    c.live_enabled = (coin == "BTC");
    if (coin == "ETH") {
        c.binance_symbol = "ETHUSDT";
        c.hourly_slug_prefix = "ethereum-up-or-down";
        c.trend_abs_dev = 0.22;
        c.trend_vol_ratio = 0.85;
        c.trend_size_usdc = 2.25;
        c.trend_strong_size_usdc = 2.50;
        c.reversal_abs_dev = 0.30;
        c.reversal_vol_ratio = 0.85;
        c.reversal_size_usdc = 2.00;
        c.reversal_strong_size_usdc = 2.25;
        c.quiet_min_dev = 0.06;
        c.quiet_max_dev = 0.16;
        c.quiet_vol_ratio = 0.65;
        c.quiet_max_entry = 0.27;
        c.quiet_size_usdc = 1.00;
    } else if (coin == "SOL") {
        c.binance_symbol = "SOLUSDT";
        c.hourly_slug_prefix = "solana-up-or-down";
        c.trend_abs_dev = 0.28;
        c.trend_vol_ratio = 0.90;
        c.trend_size_usdc = 1.75;
        c.trend_strong_size_usdc = 2.00;
        c.reversal_abs_dev = 0.38;
        c.reversal_vol_ratio = 0.90;
        c.reversal_size_usdc = 1.50;
        c.reversal_strong_size_usdc = 1.75;
        c.quiet_min_dev = 0.08;
        c.quiet_max_dev = 0.22;
        c.quiet_vol_ratio = 0.70;
        c.quiet_max_entry = 0.25;
        c.quiet_size_usdc = 0.75;
    } else if (coin == "XRP") {
        c.binance_symbol = "XRPUSDT";
        c.hourly_slug_prefix = "xrp-up-or-down";
        c.live_enabled = false;
        c.trend_abs_dev = 0.35;
        c.reversal_abs_dev = 0.45;
        c.quiet_min_dev = 0.10;
        c.quiet_max_dev = 0.25;
        c.quiet_size_usdc = 0.50;
    } else if (coin == "DOGE") {
        c.binance_symbol = "DOGEUSDT";
        c.hourly_slug_prefix = "dogecoin-up-or-down";
        c.live_enabled = false;
        c.trend_abs_dev = 0.40;
        c.reversal_abs_dev = 0.50;
        c.quiet_min_dev = 0.12;
        c.quiet_max_dev = 0.30;
        c.quiet_size_usdc = 0.50;
    } else if (coin == "BNB") {
        c.binance_symbol = "BNBUSDT";
        c.hourly_slug_prefix = "bnb-up-or-down";
        c.live_enabled = false;
        c.trend_abs_dev = 0.25;
        c.reversal_abs_dev = 0.35;
        c.quiet_min_dev = 0.07;
        c.quiet_max_dev = 0.18;
        c.quiet_size_usdc = 0.50;
    } else if (coin == "HYPE") {
        c.binance_symbol = "HYPEUSDT";
        c.hourly_slug_prefix = "hype-up-or-down";
        c.live_enabled = false;
        c.trend_abs_dev = 0.45;
        c.reversal_abs_dev = 0.60;
        c.quiet_min_dev = 0.15;
        c.quiet_max_dev = 0.35;
        c.quiet_size_usdc = 0.50;
    }
    return c;
}

AppConfig load_config(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        throw std::runtime_error("Cannot open config file: " + path);
    }

    // 先读原始字节算 config_hash（稳定标识本次运行用的配置版本），再解析。
    std::stringstream raw;
    raw << f.rdbuf();
    std::string raw_str = raw.str();
    {
        std::ostringstream hh;
        hh << std::hex << std::setw(16) << std::setfill('0')
           << std::hash<std::string>{}(raw_str);
        set_config_hash(hh.str());
    }

    json root = json::parse(raw_str);
    AppConfig cfg;

    // polymarket
    if (root.contains("polymarket")) {
        auto& p = root["polymarket"];
        cfg.polymarket.clob_rest_url = jstr(p, "clob_rest_url", "https://clob.polymarket.com");
        cfg.polymarket.clob_ws_url = jstr(p, "clob_ws_url", "wss://ws-subscriptions-clob.polymarket.com/ws/market");
        cfg.polymarket.gamma_api_url = jstr(p, "gamma_api_url", "https://gamma-api.polymarket.com");
        cfg.polymarket.chain_id = jint(p, "chain_id", 137);
        cfg.polymarket.proxy_address = jstr(p, "proxy_address");
        cfg.polymarket.api_address = jstr(p, "api_address");
    }

    // strategy
    if (root.contains("strategy")) {
        auto& s = root["strategy"];
        cfg.strategy.min_net_profit_pct = jdbl(s, "min_net_profit_pct", 0.5);
        cfg.strategy.max_trade_size_usdc = jdbl(s, "max_trade_size_usdc", 100.0);
        cfg.strategy.min_liquidity_usdc = jdbl(s, "min_liquidity_usdc", 500.0);
        cfg.strategy.min_volume_24h = jdbl(s, "min_volume_24h", 1000.0);
        cfg.strategy.max_markets_to_scan = jint(s, "max_markets_to_scan", 100);
        cfg.strategy.market_filter = jstr(s, "market_filter");
        cfg.strategy.mode = jstr(s, "mode", "dry_run");
        cfg.strategy.account_balance = jdbl(s, "account_balance", 1000.0);
        cfg.strategy.poll_interval_sec = jint(s, "poll_interval_sec", 30);
        cfg.strategy.max_global_trades_per_hour = jint(s, "max_global_trades_per_hour", 2);
        cfg.strategy.max_global_open_positions = jint(s, "max_global_open_positions", 2);
        cfg.strategy.max_quiet_trades_per_hour = jint(s, "max_quiet_trades_per_hour", 2);
        cfg.strategy.quiet_reversion_enabled = jbool(s, "quiet_reversion_enabled", false);
        if (s.contains("crypto_symbols") && s["crypto_symbols"].is_array()) {
            cfg.strategy.crypto_symbols.clear();
            for (const auto& t : s["crypto_symbols"]) {
                if (t.is_string()) cfg.strategy.crypto_symbols.push_back(upper(t.get<std::string>()));
            }
        }
        if (s.contains("arb_types") && s["arb_types"].is_array()) {
            for (const auto& t : s["arb_types"]) {
                if (t.is_string()) cfg.strategy.arb_types.push_back(t.get<std::string>());
            }
        }
    }

    // risk
    if (root.contains("risk")) {
        auto& r = root["risk"];
        cfg.risk.max_position_per_market = jdbl(r, "max_position_per_market", 500.0);
        cfg.risk.max_total_exposure = jdbl(r, "max_total_exposure", 2000.0);
        cfg.risk.max_single_trade = jdbl(r, "max_single_trade", 100.0);
        cfg.risk.daily_loss_limit = jdbl(r, "daily_loss_limit", 50.0);
        cfg.risk.max_open_orders = jint(r, "max_open_orders", 10);
        cfg.risk.kill_switch = jbool(r, "kill_switch", false);
    }

    // logging
    if (root.contains("logging")) {
        auto& l = root["logging"];
        cfg.logging.level = jstr(l, "level", "info");
        cfg.logging.file = jstr(l, "file", "./logs/bot.log");
        cfg.logging.console = jbool(l, "console", true);
    }

    // wallet
    if (root.contains("wallet")) {
        auto& w = root["wallet"];
        cfg.wallet.keystore_path = jstr(w, "keystore_path");
        cfg.wallet.address = jstr(w, "address");
    }

    // network
    if (root.contains("network")) {
        auto& n = root["network"];
        cfg.network.proxy_url = jstr(n, "proxy_url");
        cfg.network.api_host = jstr(n, "api_host", cfg.network.api_host);
        cfg.network.api_port = jint(n, "api_port", cfg.network.api_port);
    }

    // fees
    if (root.contains("fees")) {
        auto& fe = root["fees"];
        cfg.fees.maker_fee_rate = jdbl(fe, "maker_fee_rate", 0.0);
        cfg.fees.taker_fee_rate = jdbl(fe, "taker_fee_rate", 0.07);
        cfg.fees.finance_taker_fee_rate = jdbl(fe, "finance_taker_fee_rate", 0.04);
        cfg.fees.gas_per_tx_usdc = jdbl(fe, "gas_per_tx_usdc", 0.0);
        cfg.fees.min_order_usdc = jdbl(fe, "min_order_usdc", 1.0);
        cfg.fees.min_order_shares = jdbl(fe, "min_order_shares", 5.0);
    }

    // experiment: isolated paper-only strategy simulator
    if (root.contains("experiment")) {
        auto& ex = root["experiment"];
        cfg.experiment.enabled = jbool(ex, "enabled", cfg.experiment.enabled);
        cfg.experiment.strategy = jstr(ex, "strategy", cfg.experiment.strategy);
        cfg.experiment.initial_balance = jdbl(ex, "initial_balance", cfg.experiment.initial_balance);
    }

    // polygon (live 模式必填；rpc_urls 默认填一组公开免费节点；合约地址用 struct 默认值)
    if (root.contains("polygon")) {
        auto& pg = root["polygon"];
        if (pg.contains("rpc_urls") && pg["rpc_urls"].is_array()) {
            for (const auto& u : pg["rpc_urls"]) {
                if (u.is_string()) cfg.polygon.rpc_urls.push_back(u.get<std::string>());
            }
        }
        cfg.polygon.chain_id = jint(pg, "chain_id", 137);
        cfg.polygon.usdc_e_address = jstr(pg, "usdc_e_address", cfg.polygon.usdc_e_address);
        cfg.polygon.usdc_native_address = jstr(pg, "usdc_native_address", cfg.polygon.usdc_native_address);
        cfg.polygon.ctf_address = jstr(pg, "ctf_address", cfg.polygon.ctf_address);
        cfg.polygon.ctf_exchange = jstr(pg, "ctf_exchange", cfg.polygon.ctf_exchange);
        cfg.polygon.neg_risk_exchange = jstr(pg, "neg_risk_exchange", cfg.polygon.neg_risk_exchange);
        cfg.polygon.neg_risk_adapter = jstr(pg, "neg_risk_adapter", cfg.polygon.neg_risk_adapter);
    }
    if (cfg.polygon.rpc_urls.empty()) {
        cfg.polygon.rpc_urls = {
            "https://polygon-bor-rpc.publicnode.com",
            "https://polygon.drpc.org",
            "https://1rpc.io/matic"
        };
    }

    cfg.coins.clear();
    for (const auto& sym : cfg.strategy.crypto_symbols) {
        cfg.coins.push_back(default_coin_config(sym));
    }
    if (root.contains("coins") && root["coins"].is_array()) {
        for (const auto& cj : root["coins"]) {
            std::string coin = upper(jstr(cj, "coin", ""));
            if (coin.empty()) continue;
            CoinStrategyConfig cc = default_coin_config(coin);
            cc.binance_symbol = jstr(cj, "binance_symbol", cc.binance_symbol);
            cc.hourly_slug_prefix = jstr(cj, "hourly_slug_prefix", cc.hourly_slug_prefix);
            cc.live_enabled = jbool(cj, "live_enabled", cc.live_enabled);
            cc.trend_abs_dev = jdbl(cj, "trend_abs_dev", cc.trend_abs_dev);
            cc.trend_vol_ratio = jdbl(cj, "trend_vol_ratio", cc.trend_vol_ratio);
            cc.trend_size_usdc = jdbl(cj, "trend_size_usdc", cc.trend_size_usdc);
            cc.trend_strong_size_usdc = jdbl(cj, "trend_strong_size_usdc", cc.trend_strong_size_usdc);
            cc.reversal_abs_dev = jdbl(cj, "reversal_abs_dev", cc.reversal_abs_dev);
            cc.reversal_vol_ratio = jdbl(cj, "reversal_vol_ratio", cc.reversal_vol_ratio);
            cc.reversal_size_usdc = jdbl(cj, "reversal_size_usdc", cc.reversal_size_usdc);
            cc.reversal_strong_size_usdc = jdbl(cj, "reversal_strong_size_usdc", cc.reversal_strong_size_usdc);
            cc.quiet_min_dev = jdbl(cj, "quiet_min_dev", cc.quiet_min_dev);
            cc.quiet_max_dev = jdbl(cj, "quiet_max_dev", cc.quiet_max_dev);
            cc.quiet_vol_ratio = jdbl(cj, "quiet_vol_ratio", cc.quiet_vol_ratio);
            cc.quiet_max_entry = jdbl(cj, "quiet_max_entry", cc.quiet_max_entry);
            cc.quiet_size_usdc = jdbl(cj, "quiet_size_usdc", cc.quiet_size_usdc);
            cc.max_spread = jdbl(cj, "max_spread", cc.max_spread);
            bool found = false;
            for (auto& existing : cfg.coins) {
                if (existing.coin == coin) {
                    existing = cc;
                    found = true;
                    break;
                }
            }
            if (!found) cfg.coins.push_back(cc);
        }
    }
    if (cfg.coins.empty()) cfg.coins.push_back(default_coin_config("BTC"));

    return cfg;
}

void init_logging(const LoggingConfig& cfg) {
    std::vector<spdlog::sink_ptr> sinks;

    if (cfg.console) {
        sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    }

    if (!cfg.file.empty()) {
        // 确保日志目录存在
        auto dir = std::filesystem::path(cfg.file).parent_path();
        if (!dir.empty()) {
            std::filesystem::create_directories(dir);
        }
        // 10MB 轮转, 保留 3 个文件
        sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            cfg.file, 10 * 1024 * 1024, 3));
    }

    auto logger = std::make_shared<spdlog::logger>("main", sinks.begin(), sinks.end());

    // 设置日志级别
    if (cfg.level == "debug") logger->set_level(spdlog::level::debug);
    else if (cfg.level == "info") logger->set_level(spdlog::level::info);
    else if (cfg.level == "warn") logger->set_level(spdlog::level::warn);
    else if (cfg.level == "error") logger->set_level(spdlog::level::err);
    else logger->set_level(spdlog::level::info);

    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");

    spdlog::set_default_logger(logger);
    spdlog::flush_every(std::chrono::seconds(3));
}

}  // namespace polymarket
