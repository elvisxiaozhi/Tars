#pragma once

#include <string>
#include <vector>

namespace polymarket {

struct PolymarketConfig {
    std::string clob_rest_url;
    std::string clob_ws_url;
    std::string gamma_api_url;
    int chain_id = 137;
};

struct StrategyConfig {
    double min_net_profit_pct = 0.5;
    double max_trade_size_usdc = 100.0;
    std::vector<std::string> arb_types;
    double min_liquidity_usdc = 500.0;
    double min_volume_24h = 1000.0;
    int max_markets_to_scan = 100;
    std::string market_filter;  // 关键词过滤，空 = 不过滤
};

struct RiskConfig {
    double max_position_per_market = 500.0;
    double max_total_exposure = 2000.0;
    double max_single_trade = 100.0;
    double daily_loss_limit = 50.0;
    int max_open_orders = 10;
    bool kill_switch = false;
};

struct LoggingConfig {
    std::string level = "info";
    std::string file = "./logs/bot.log";
    bool console = true;
};

struct WalletConfig {
    std::string keystore_path;
    std::string address;
};

struct NetworkConfig {
    std::string proxy_url;  // e.g. "http://127.0.0.1:7897", empty = use env var
};

struct AppConfig {
    PolymarketConfig polymarket;
    StrategyConfig strategy;
    RiskConfig risk;
    LoggingConfig logging;
    WalletConfig wallet;
    NetworkConfig network;
};

// 从 JSON 文件加载配置
AppConfig load_config(const std::string& path);

// 初始化日志系统（根据 LoggingConfig）
void init_logging(const LoggingConfig& cfg);

}  // namespace polymarket
