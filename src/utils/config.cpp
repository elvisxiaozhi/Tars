#include "utils/config.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>

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

AppConfig load_config(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        throw std::runtime_error("Cannot open config file: " + path);
    }

    json root = json::parse(f);
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
        cfg.network.api_port = jint(n, "api_port", 9090);
    }

    // fees
    if (root.contains("fees")) {
        auto& fe = root["fees"];
        cfg.fees.maker_fee_rate = jdbl(fe, "maker_fee_rate", 0.0);
        cfg.fees.taker_fee_rate = jdbl(fe, "taker_fee_rate", 0.072);
        cfg.fees.gas_per_tx_usdc = jdbl(fe, "gas_per_tx_usdc", 0.0);
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
