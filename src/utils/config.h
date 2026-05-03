#pragma once

#include <string>
#include <vector>

namespace polymarket {

struct PolymarketConfig {
    std::string clob_rest_url;
    std::string clob_ws_url;
    std::string gamma_api_url;
    int chain_id = 137;

    // Polymarket 3 层账户架构（live 模式必填，dry_run 可空）
    // - wallet.address (EOA)：你的私钥派生地址，签 EIP-712
    // - proxy_address：Polymarket 部署的 Simple7702Account（持 USDC + CTF，订单 maker）
    // - api_address：CLOB API 鉴权用（profile 页"API use only"那个）
    std::string proxy_address;
    std::string api_address;
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
    std::vector<std::string> crypto_symbols = {"BTC", "ETH", "SOL", "XRP", "DOGE", "BNB", "HYPE"};
    int max_global_trades_per_hour = 2;
    int max_global_open_positions = 2;
    int max_quiet_trades_per_hour = 2;
};

struct CoinStrategyConfig {
    std::string coin = "BTC";
    std::string binance_symbol = "BTCUSDT";
    std::string hourly_slug_prefix = "bitcoin-up-or-down";
    bool live_enabled = false;

    double trend_abs_dev = 0.18;
    double trend_vol_ratio = 0.80;
    double trend_size_usdc = 2.25;
    double trend_strong_size_usdc = 2.50;

    double reversal_abs_dev = 0.25;
    double reversal_vol_ratio = 0.80;
    double reversal_size_usdc = 2.25;
    double reversal_strong_size_usdc = 2.50;

    double quiet_min_dev = 0.04;
    double quiet_max_dev = 0.12;
    double quiet_vol_ratio = 0.60;
    double quiet_max_entry = 0.28;
    double quiet_size_usdc = 1.25;
    double max_spread = 0.04;
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

// Polygon 链上读取所需参数（live 模式必填）
// 合约地址 = Polymarket 主网地址（截至 2026-04，硬编码默认值，配置可覆盖）
struct PolygonConfig {
    // RPC 端点列表（按顺序 failover；调用失败自动切换下一个）
    std::vector<std::string> rpc_urls;
    int chain_id = 137;             // 137 = Polygon mainnet, 80002 = Amoy testnet
    // ERC-20: USDC.e (Polymarket 历史用) + USDC native (2024 后迁移)
    std::string usdc_e_address      = "0x2791Bca1f2de4661ED88A30C99A7a9449Aa84174";
    std::string usdc_native_address = "0x3c499c542cEF5E3811e1192ce70d8cC03d5c3359";
    // ERC-1155: ConditionalTokens 合约（持有所有 outcome token）
    std::string ctf_address         = "0x4D97DCd97eC945f40cF65F87097ACe5EA0476045";
    // CTFExchange（binary 二元期权撮合 + 结算合约；BTC UP/DOWN 用这个）
    std::string ctf_exchange        = "0x4bFb41d5B3570DeFd03C39a9A4D8dE6Bd8B8982E";
    // NegRiskCTFExchange + Adapter（多 outcome 市场，先存着 R7+ 可能用到）
    std::string neg_risk_exchange   = "0xC5d563A36AE78145C45a50134d48A1215220f80a";
    std::string neg_risk_adapter    = "0xd91E80cF2E7be2e162c6513ceD06f1dD0dA35296";
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
    std::vector<CoinStrategyConfig> coins;
};

CoinStrategyConfig default_coin_config(const std::string& coin);

// 从 JSON 文件加载配置
AppConfig load_config(const std::string& path);

// 初始化日志系统（根据 LoggingConfig）
void init_logging(const LoggingConfig& cfg);

}  // namespace polymarket
