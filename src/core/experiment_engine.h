#pragma once

#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <utility>
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

struct TrendFollowContext {
    bool has_btc = false;
    bool has_eth = false;
    BtcMarketData btc;
    BtcMarketData eth;
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
                                      const std::string& question,
                                      const TrendFollowContext& ctx);
    // trend_v3：trend_v2 基底 + dev-accel 门解锁高价带（0.68-0.79 需 ~90s dev 上升）。
    // 见 docs/steps/step-trend-v3-shadow.md。需要 now_ms 维护 dev 历史窗口。
    EntrySignal evaluate_trend_v3(const BtcMarketData& md,
                                  const ExperimentQuotes& quotes,
                                  const std::string& condition_id,
                                  const std::string& question,
                                  const TrendFollowContext& ctx,
                                  int64_t now_ms);
    EntrySignal evaluate_legacy_cheap_v2(const BtcMarketData& md,
                                         const ExperimentQuotes& quotes,
                                         const std::string& condition_id,
                                         const std::string& question,
                                         const TrendFollowContext& ctx);
    EntrySignal evaluate_eth_cheap_v1(const BtcMarketData& md,
                                      const ExperimentQuotes& quotes,
                                      const std::string& condition_id,
                                      const std::string& question);
    EntrySignal evaluate_eth_only_v1(const BtcMarketData& md,
                                     const ExperimentQuotes& quotes,
                                     const std::string& condition_id,
                                     const std::string& question);
    EntrySignal evaluate_late_window_v1(const BtcMarketData& md,
                                        const ExperimentQuotes& quotes,
                                        const std::string& condition_id,
                                        const std::string& question);
    EntrySignal evaluate_eth_late_cheap_v1(const BtcMarketData& md,
                                           const ExperimentQuotes& quotes,
                                           const std::string& condition_id,
                                           const std::string& question);
    EntrySignal evaluate_finance_updown_v1(const BtcMarketData& md,
                                           const ExperimentQuotes& quotes,
                                           const std::string& condition_id,
                                           const std::string& question);
    EntrySignal evaluate_crypto_duration_updown_v1(const BtcMarketData& md,
                                                   const ExperimentQuotes& quotes,
                                                   const std::string& condition_id,
                                                   const std::string& question,
                                                   bool daily);
    // held-to-expiry shadow（近零费：入场 maker + 到期结算无离场费）。买入后不设 TP/止损，
    // 持有到 1h 结算（0/1）。held_favorite：晚段(剩≤12min)买确定性赢家(0.80-0.93)；
    // held_momentum：中段(20-40min)买便宜顺势侧(0.45-0.62)。见 docs/strategy-experiments.md。
    EntrySignal evaluate_held_favorite_v1(const BtcMarketData& md,
                                          const ExperimentQuotes& quotes,
                                          const std::string& condition_id,
                                          const std::string& question);
    EntrySignal evaluate_held_momentum_v1(const BtcMarketData& md,
                                          const ExperimentQuotes& quotes,
                                          const std::string& condition_id,
                                          const std::string& question);
    // contrarian_hold_v1：中段买便宜 underdog(dev 反方向侧, 0.38-0.48)持有到期，赌反转。
    // 是 held_momentum 的镜像（favored 侧实测 43%<价 → underdog +EV，低确认+regime 依赖，待验证）。
    EntrySignal evaluate_contrarian_hold_v1(const BtcMarketData& md,
                                            const ExperimentQuotes& quotes,
                                            const std::string& condition_id,
                                            const std::string& question);

    std::vector<TakeProfitLevel> compute_tp_levels(double entry_price,
                                                   StrategyRegime regime) const;
    std::vector<TakeProfitLevel> compute_eth_only_tp_levels(double entry_price,
                                                            StrategyRegime regime) const;
    std::vector<TakeProfitLevel> compute_late_window_tp_levels(double entry_price) const;
    std::vector<TakeProfitLevel> compute_eth_late_cheap_tp_levels(double entry_price) const;
    std::vector<TakeProfitLevel> compute_finance_updown_tp_levels(double entry_price,
                                                                  StrategyRegime regime) const;
    std::vector<TakeProfitLevel> compute_crypto_duration_tp_levels(double entry_price,
                                                                   bool daily) const;
    std::vector<TakeProfitLevel> compute_trend_follow_tp_levels(double entry_price) const;

    ExitSignal evaluate_exit(const Position& pos,
                             double current_contract_price,
                             const BtcMarketData& md) const;
    ExitSignal evaluate_trend_follow_exit(const Position& pos,
                                          double current_contract_price,
                                          const BtcMarketData& md,
                                          const TrendFollowContext& ctx) const;
    ExitSignal evaluate_eth_only_exit(const Position& pos,
                                      double current_contract_price,
                                      const BtcMarketData& md) const;
    ExitSignal evaluate_late_window_exit(const Position& pos,
                                         double current_contract_price,
                                         const BtcMarketData& md) const;
    ExitSignal evaluate_eth_late_cheap_exit(const Position& pos,
                                            double current_contract_price,
                                            const BtcMarketData& md) const;
    ExitSignal evaluate_finance_updown_exit(const Position& pos,
                                            double current_contract_price,
                                            const BtcMarketData& md) const;
    ExitSignal evaluate_crypto_duration_exit(const Position& pos,
                                             double current_contract_price,
                                             const BtcMarketData& md,
                                             bool daily) const;

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
    // trend_v3 的 dev-accel 门：按 condition_id 维护 (now_ms, abs_dev) 历史，修剪到 ~95s 窗口。
    std::deque<std::pair<int64_t, double>> tv3_dev_hist_;
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
    void record_candidate(const std::string& coin,
                          const BtcMarketData& md,
                          const MarketEntry& entry,
                          const ExperimentQuotes& quotes,
                          const EntrySignal& sig,
                          int64_t now_ms) const;
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
    std::string candidate_log_path_;
    std::string id_prefix_;
    std::map<std::string, ExperimentStrategy> strategies_;
    std::map<std::string, BtcMarketData> latest_market_data_;
    std::map<std::string, CoinRiskState> coin_state_;
    std::vector<Position> positions_;
    std::vector<TradeRecord> trades_;
    std::map<std::string, RegimeStats> regime_stats_;
    std::map<std::string, int> reject_counts_;
    std::string last_signal_;
    mutable std::mutex mu_;
};

}  // namespace polymarket
