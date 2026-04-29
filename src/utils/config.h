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
    std::string market_filter;       // 关键词过滤，空 = 不过滤
    std::string mode = "dry_run";    // "dry_run" or "live"
    double account_balance = 1000.0;
    int poll_interval_sec = 30;      // 策略循环间隔
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
    int api_port = 9090;    // 内嵌 dashboard HTTP 端口
};

// Live 交易所需的链上参数（mode=="live" 时强制要求；dry_run 不读）
// 合约地址需用户从 docs.polymarket.com → "Contract Addresses" 填入
struct PolygonConfig {
    std::string rpc_url;            // e.g. "https://polygon-rpc.com"
    int chain_id = 137;             // 137 = Polygon mainnet, 80002 = Amoy testnet
    std::string usdc_address;       // USDC.e (Polymarket 计价币)
    std::string ctf_address;        // ConditionalTokens (ERC-1155)
    std::string exchange_address;   // CTFExchange（订单结算合约）
};

// Polymarket 实际费率（截至 2026-04 docs.polymarket.com）：
// maker 0%；Crypto category taker 7.2%；公式 fee = shares × rate × p × (1-p)
struct FeeConfig {
    double maker_fee_rate = 0.0;
    double taker_fee_rate = 0.072;
    double gas_per_tx_usdc = 0.0;
};

struct AppConfig {
    PolymarketConfig polymarket;
    StrategyConfig strategy;
    RiskConfig risk;
    LoggingConfig logging;
    WalletConfig wallet;
    NetworkConfig network;
    FeeConfig fees;
    PolygonConfig polygon;
};

// 从 JSON 文件加载配置
AppConfig load_config(const std::string& path);

// 初始化日志系统（根据 LoggingConfig）
void init_logging(const LoggingConfig& cfg);

}  // namespace polymarket
