#pragma once

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <json.hpp>

#include "core/binance_feed.h"
#include "core/market_feed.h"
#include "core/strategy.h"
#include "core/trade_journal.h"
#include "utils/config.h"

namespace polymarket {

struct ExperimentQuotes {
    double up_bid = 0;
    double up_ask = 0;
    double down_bid = 0;
    double down_ask = 0;
    std::string up_token_id;
    std::string down_token_id;
};

class ExperimentStrategy {
public:
    ExperimentStrategy(const AppConfig& cfg, std::string coin);

    EntrySignal evaluate_entry(const BtcMarketData& md,
                               const ExperimentQuotes& quotes,
                               const std::string& condition_id,
                               const std::string& question);
    EntrySignal evaluate_trend_follow(const BtcMarketData& md,
                                      const ExperimentQuotes& quotes,
                                      const std::string& condition_id,
                                      const std::string& question);

    std::vector<TakeProfitLevel> compute_tp_levels(double entry_price,
                                                   StrategyRegime regime) const;
    std::vector<TakeProfitLevel> compute_trend_follow_tp_levels(double entry_price) const;

    ExitSignal evaluate_exit(const Position& pos,
                             double current_contract_price,
                             const BtcMarketData& md) const;
    ExitSignal evaluate_trend_follow_exit(const Position& pos,
                                          double current_contract_price,
                                          const BtcMarketData& md) const;

private:
    const AppConfig& cfg_;
    std::string coin_;
    CoinStrategyConfig params_;
    std::string last_condition_id_;
    double abs_dev_prev2_ = 0;
    double abs_dev_prev1_ = 0;
    bool has_dev_prev2_ = false;
    bool has_dev_prev1_ = false;
    Side trend_prev_side_ = Side::NONE;
    bool has_trend_prev_side_ = false;
};

class ExperimentEngine {
public:
    explicit ExperimentEngine(const AppConfig& cfg,
                              std::string strategy_name = "regime",
                              std::string log_path = "./logs/experiment_trades.jsonl",
                              std::string id_prefix = "E");

    void reset_candle(const std::string& coin);
    void reset_global_hour();

    void on_market(const std::string& coin,
                   const BtcMarketData& md,
                   const MarketEntry& entry,
                   const ExperimentQuotes& quotes,
                   int64_t now_ms);

    void prune_closed();
    std::string status_json() const;
    std::string trades_json() const;
    std::string all_trades_json() const;

private:
    struct CoinRiskState {
        bool candle_stopped = false;
        bool candle_traded = false;
    };

    struct RegimeStats {
        int trades = 0;
        int wins = 0;
        int losses = 0;
        double pnl = 0;
    };

    bool has_open_coin(const std::string& coin) const;
    void record_trade(const TradeRecord& rec);
    void fill_record_analytics(TradeRecord& rec, const Position& pos,
                               const BtcMarketData& md, int64_t now_ms) const;

    const AppConfig& cfg_;
    bool enabled_ = true;
    double balance_ = 20.0;
    int next_id_ = 1;
    int global_trades_this_hour_ = 0;
    int quiet_trades_this_hour_ = 0;
    int non_btc_stop_price_this_hour_ = 0;
    std::string strategy_name_;
    std::string log_path_;
    std::string id_prefix_;
    std::map<std::string, ExperimentStrategy> strategies_;
    std::map<std::string, CoinRiskState> coin_state_;
    std::vector<Position> positions_;
    std::vector<TradeRecord> trades_;
    std::map<std::string, RegimeStats> regime_stats_;
    std::map<std::string, int> reject_counts_;
    std::string last_signal_;
    mutable std::mutex mu_;
};

}  // namespace polymarket
