#pragma once

#include <string>
#include <vector>

#include "core/binance_feed.h"
#include "utils/config.h"

namespace polymarket {

// 交易方向
enum class Side { UP, DOWN, NONE };

enum class StrategyRegime { LEGACY_CHEAP, TREND, REVERSAL, QUIET_REVERSION, NONE };

// 入场信号
struct EntrySignal {
    bool valid = false;
    Side side = Side::NONE;
    double entry_price = 0;       // 建议入场价（ask 价下方 0.5-1¢）
    double market_ask = 0;        // 当前 ask 价
    double size_usdc = 0;         // 目标投入金额
    double shares = 0;            // 目标份数
    std::string coin = "BTC";
    StrategyRegime regime = StrategyRegime::LEGACY_CHEAP;
    std::string market_question;
    std::string token_id;
    std::string condition_id;
    std::string reject_reason;    // 如果 valid=false，为什么被拒
    int entry_confidence = 0;
    std::string btc_alignment;
    std::string eth_alignment;
    std::string cross_coin_state;
    std::string entry_price_bucket;
    std::string confidence_components;
};

// 止盈档位
struct TakeProfitLevel {
    int tier;              // 1-6
    double trigger_price;  // 触发价
    double sell_pct;       // 卖出比例 (0-1)，基于剩余仓位
    bool triggered = false;
};

// 模拟持仓
struct Position {
    std::string id;               // 唯一ID
    Side side = Side::NONE;
    std::string token_id;
    std::string condition_id;
    std::string market_question;
    std::string coin = "BTC";
    StrategyRegime regime = StrategyRegime::LEGACY_CHEAP;
    double entry_price = 0;
    double current_price = 0;
    double size_usdc = 0;         // 投入金额
    double shares = 0;            // 持有份数 = size_usdc / entry_price
    double btc_price_at_entry = 0;
    double btc_strike_at_entry = 0;
    double entry_vol = 0;         // 入场时波动率
    double avg_vol = 0;           // 入场时24h平均波动率
    double entry_fee = 0;         // 买入手续费（用于 P&L 计算）
    int64_t entry_time = 0;       // unix ms
    int minutes_remaining_at_entry = 0;
    std::vector<TakeProfitLevel> tp_levels;
    double shares_remaining_pct = 1.0;  // 剩余仓位比例

    // --- Analytics fields ---
    double max_price = 0;              // 持仓期间合约最高 bid
    double min_price = 1e9;            // 持仓期间合约最低 bid
    // 入场后 N 分钟时间窗口内的最高价快照（用于死水早退规则的离线评估）
    double mfe_at_5min = 0;
    double mfe_at_10min = 0;
    double mfe_at_15min = 0;
    double spread_at_entry = 0;        // 入场时买卖价差
    double ask_depth_at_entry = 0;     // 入场时 ask 侧总挂单量
    int hour_et = -1;                  // 入场小时 (ET, 0-23)
    int day_of_week = -1;             // 入场星期 (0=Sun..6=Sat)
    int consec_wins_before = 0;        // 入场前连赢次数
    int consec_losses_before = 0;      // 入场前连亏次数
    double balance_before = 0;         // 入场前账户余额
    int entry_confidence = 0;
    std::string btc_alignment;
    std::string eth_alignment;
    std::string cross_coin_state;
    std::string entry_price_bucket;
    std::string confidence_components;

    bool closed = false;
    std::string close_reason;     // "tp0"/"tp1"/"tp2"/"stop_price"/"trailing_stop"/"stop_btc"/"stop_time"/"expired"
    double realized_pnl = 0;
};

// 策略退出信号
struct ExitSignal {
    bool should_exit = false;
    std::string reason;
    double exit_price = 0;
    bool use_market_order = false;  // true = 止损用市价单
};

class Strategy {
public:
    explicit Strategy(const AppConfig& cfg);
    Strategy(const AppConfig& cfg, std::string coin);

    // 评估入场条件，返回信号
    EntrySignal evaluate_entry(
        const BtcMarketData& btc,
        double up_bid, double up_ask,
        double down_bid, double down_ask,
        const std::string& up_token_id, const std::string& down_token_id,
        const std::string& condition_id, const std::string& question,
        int minutes_remaining);

    // 计算止盈档位
    std::vector<TakeProfitLevel> compute_tp_levels(double entry_price);
    std::vector<TakeProfitLevel> compute_tp_levels(double entry_price, StrategyRegime regime);

    // 评估是否应该退出
    ExitSignal evaluate_exit(
        const Position& pos,
        double current_contract_price,
        const BtcMarketData& btc,
        int minutes_remaining);

    // 最后10分钟特殊处理
    ExitSignal evaluate_last_10min(
        const Position& pos,
        double current_contract_price,
        int minutes_remaining);

private:
    // 根据剩余时间返回最大入场价
    double max_entry_price(int minutes_remaining) const;
    EntrySignal evaluate_cheap_rebound(
        const BtcMarketData& btc,
        double up_bid, double up_ask,
        double down_bid, double down_ask,
        const std::string& up_token_id, const std::string& down_token_id,
        const std::string& condition_id, const std::string& question,
        int minutes_remaining);
    EntrySignal evaluate_momentum_follow(
        const BtcMarketData& btc,
        double up_bid, double up_ask,
        double down_bid, double down_ask,
        const std::string& up_token_id, const std::string& down_token_id,
        const std::string& condition_id, const std::string& question,
        int minutes_remaining);

    AppConfig cfg_;
    std::string coin_ = "BTC";
};

}  // namespace polymarket
