#include "core/experiment_engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>

#include <spdlog/spdlog.h>

namespace polymarket {

namespace {

const char* regime_name_local(StrategyRegime regime) {
    switch (regime) {
        case StrategyRegime::TREND: return "trend";
        case StrategyRegime::REVERSAL: return "reversal";
        case StrategyRegime::QUIET_REVERSION: return "quiet_reversion";
        case StrategyRegime::LEGACY_CHEAP: return "legacy_cheap";
        default: return "none";
    }
}

int64_t wall_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string side_name(Side side) {
    return side == Side::UP ? "UP" : side == Side::DOWN ? "DOWN" : "NONE";
}

struct LegacyV2Filter {
    double min_entry = 0.18;
    double max_entry = 0.26;
    double max_abs_dev = 0.30;
    double max_spread = 0.03;
};

LegacyV2Filter legacy_v2_filter_for_coin(const std::string& coin) {
    if (coin == "BTC" || coin == "SOL") {
        return {0.18, 0.26, 0.30, 0.03};
    }
    if (coin == "ETH") {
        return {0.20, 0.26, 0.25, 0.02};
    }
    return {0.20, 0.24, 0.20, 0.02};
}

bool trend_follow_side_aligned(Side side, double deviation_pct) {
    return (side == Side::UP && deviation_pct > 0) ||
           (side == Side::DOWN && deviation_pct < 0);
}

double fee(double shares, double price, bool taker, const FeeConfig& fees) {
    double rate = taker ? fees.taker_fee_rate : fees.maker_fee_rate;
    return shares * rate * price * (1.0 - price) + fees.gas_per_tx_usdc;
}

void append_jsonl(const std::string& path, const TradeRecord& rec) {
    nlohmann::json j;
    j["id"] = rec.id;
    j["mode"] = rec.mode;
    j["coin"] = rec.coin;
    j["regime"] = rec.regime;
    j["market"] = rec.market_question;
    j["side"] = rec.side;
    j["entry_time"] = rec.entry_time;
    j["exit_time"] = rec.exit_time;
    j["minutes_remaining"] = rec.minutes_remaining_at_entry;
    j["entry_price"] = rec.entry_price;
    j["exit_price"] = rec.exit_price;
    j["size_usdc"] = rec.size_usdc;
    j["shares"] = rec.shares;
    j["exit_reason"] = rec.exit_reason;
    j["pnl"] = rec.realized_pnl;
    j["fee"] = rec.fee_paid;
    j["max_price"] = rec.max_price;
    j["min_price"] = rec.min_price;
    j["btc_price"] = rec.btc_price_at_entry;
    j["btc_strike"] = rec.btc_strike;
    j["btc_deviation_pct"] = rec.btc_deviation_pct;
    j["entry_vol"] = rec.entry_vol;
    j["avg_vol"] = rec.avg_vol;
    j["btc_price_at_exit"] = rec.btc_price_at_exit;
    j["btc_deviation_at_exit"] = rec.btc_deviation_at_exit;
    j["spread_at_entry"] = rec.spread_at_entry;
    j["hold_duration_sec"] = rec.hold_duration_sec;
    j["mfe_capture_rate"] = rec.mfe_capture_rate;
    j["mfe_at_5min"] = rec.mfe_at_5min;
    j["mfe_at_10min"] = rec.mfe_at_10min;
    j["mfe_at_15min"] = rec.mfe_at_15min;
    j["mfe5_gain_pct"] = rec.mfe5_gain_pct;
    j["mfe10_gain_pct"] = rec.mfe10_gain_pct;
    j["mfe15_gain_pct"] = rec.mfe15_gain_pct;
    std::ofstream out(path, std::ios::app);
    if (out) out << j.dump() << "\n";
}

}  // namespace

ExperimentStrategy::ExperimentStrategy(const AppConfig& cfg, std::string coin)
    : cfg_(cfg), coin_(std::move(coin)), params_(default_coin_config(coin_)) {
    for (const auto& c : cfg_.coins) {
        if (c.coin == coin_) {
            params_ = c;
            break;
        }
    }
}

EntrySignal ExperimentStrategy::evaluate_entry(const BtcMarketData& md,
                                               const ExperimentQuotes& quotes,
                                               const std::string& condition_id,
                                               const std::string& question) {
    EntrySignal sig;
    sig.condition_id = condition_id;
    sig.market_question = question;
    sig.coin = coin_;

    double abs_dev = std::abs(md.deviation_pct);
    if (condition_id != last_condition_id_) {
        last_condition_id_ = condition_id;
        abs_dev_prev1_ = abs_dev;
        abs_dev_prev2_ = abs_dev;
        has_dev_prev1_ = true;
        has_dev_prev2_ = false;
        sig.reject_reason = "regime_warmup";
        return sig;
    }

    bool has_slope = has_dev_prev1_ && has_dev_prev2_;
    constexpr double kSlopeEps = 0.005;
    bool expanding = has_slope &&
        (abs_dev > abs_dev_prev1_ + kSlopeEps) &&
        (abs_dev_prev1_ > abs_dev_prev2_ + kSlopeEps);
    bool contracting = has_slope &&
        (abs_dev + kSlopeEps < abs_dev_prev1_) &&
        (abs_dev_prev1_ + kSlopeEps < abs_dev_prev2_);

    abs_dev_prev2_ = abs_dev_prev1_;
    abs_dev_prev1_ = abs_dev;
    has_dev_prev2_ = has_dev_prev1_;
    has_dev_prev1_ = true;

    double vol_ratio = md.avg_24h_vol > 1e-9 ? md.current_1h_vol / md.avg_24h_vol : 0.0;
    if (!has_slope) {
        sig.reject_reason = "regime_warmup";
        return sig;
    }

    Side candidate_side = Side::NONE;
    double candidate_ask = 0;
    std::string candidate_token;
    StrategyRegime regime = StrategyRegime::NONE;
    double size_usdc = 0;

    if (abs_dev >= params_.trend_abs_dev && expanding &&
        vol_ratio >= params_.trend_vol_ratio &&
        md.minutes_remaining >= 35 && md.minutes_remaining <= 45) {
        candidate_side = md.deviation_pct > 0 ? Side::UP : Side::DOWN;
        candidate_ask = candidate_side == Side::UP ? quotes.up_ask : quotes.down_ask;
        candidate_token = candidate_side == Side::UP ? quotes.up_token_id : quotes.down_token_id;
        regime = StrategyRegime::TREND;
        size_usdc = (abs_dev >= params_.trend_abs_dev + 0.07 && vol_ratio >= 1.0)
            ? params_.trend_strong_size_usdc : params_.trend_size_usdc;
        if (candidate_ask < 0.50 || candidate_ask > 0.75) {
            sig.reject_reason = "trend_price_window";
            return sig;
        }
    } else if (abs_dev >= params_.reversal_abs_dev && contracting &&
               vol_ratio <= params_.reversal_vol_ratio &&
               md.minutes_remaining >= 30 && md.minutes_remaining <= 38) {
        candidate_side = md.deviation_pct > 0 ? Side::DOWN : Side::UP;
        candidate_ask = candidate_side == Side::UP ? quotes.up_ask : quotes.down_ask;
        candidate_token = candidate_side == Side::UP ? quotes.up_token_id : quotes.down_token_id;
        regime = StrategyRegime::REVERSAL;
        size_usdc = abs_dev >= params_.reversal_abs_dev + 0.10
            ? params_.reversal_strong_size_usdc : params_.reversal_size_usdc;
        if (candidate_ask > 0.30) {
            sig.reject_reason = "reversal_ask_too_high";
            return sig;
        }
    } else if (abs_dev >= params_.quiet_min_dev && abs_dev <= params_.quiet_max_dev &&
               vol_ratio <= params_.quiet_vol_ratio &&
               md.minutes_remaining >= 25 && md.minutes_remaining <= 35) {
        candidate_side = quotes.up_ask <= quotes.down_ask ? Side::UP : Side::DOWN;
        candidate_ask = candidate_side == Side::UP ? quotes.up_ask : quotes.down_ask;
        candidate_token = candidate_side == Side::UP ? quotes.up_token_id : quotes.down_token_id;
        regime = StrategyRegime::QUIET_REVERSION;
        size_usdc = params_.quiet_size_usdc;
        double spread = (quotes.up_ask > 0 && quotes.down_ask > 0)
            ? std::abs((quotes.up_ask + quotes.down_ask) - 1.0) : 0.0;
        if (candidate_ask > params_.quiet_max_entry) {
            sig.reject_reason = "quiet_ask_too_high";
            return sig;
        }
        if (spread > params_.max_spread) {
            sig.reject_reason = "quiet_spread_wide";
            return sig;
        }
    } else {
        sig.reject_reason = "no_regime";
        return sig;
    }

    if (candidate_ask <= 0 || candidate_ask >= 1.0 || size_usdc <= 0) {
        sig.reject_reason = "invalid_signal";
        return sig;
    }

    sig.valid = true;
    sig.side = candidate_side;
    sig.regime = regime;
    sig.market_ask = candidate_ask;
    sig.entry_price = std::max(0.01, candidate_ask - 0.01);
    sig.size_usdc = size_usdc;
    sig.shares = size_usdc / sig.entry_price;
    sig.token_id = candidate_token;
    return sig;
}

EntrySignal ExperimentStrategy::evaluate_trend_follow(
    const BtcMarketData& md,
    const ExperimentQuotes& quotes,
    const std::string& condition_id,
    const std::string& question) {
    EntrySignal sig;
    sig.condition_id = condition_id;
    sig.market_question = question;
    sig.coin = coin_;
    sig.regime = StrategyRegime::TREND;

    if (condition_id != last_condition_id_) {
        last_condition_id_ = condition_id;
        trend_prev_side_ = Side::NONE;
        has_trend_prev_side_ = false;
    }

    if (md.minutes_remaining <= 30) {
        sig.reject_reason = "time_too_short";
        return sig;
    }
    if (md.minutes_remaining > 42) {
        sig.reject_reason = "trend_follow_too_early";
        return sig;
    }

    Side side = md.deviation_pct > 0 ? Side::UP :
                md.deviation_pct < 0 ? Side::DOWN : Side::NONE;
    if (side == Side::NONE) {
        sig.reject_reason = "no_direction";
        return sig;
    }

    bool direction_confirmed = has_trend_prev_side_ && trend_prev_side_ == side;
    trend_prev_side_ = side;
    has_trend_prev_side_ = true;
    if (!direction_confirmed) {
        sig.reject_reason = "trend_follow_direction_not_confirmed";
        return sig;
    }

    double abs_dev = std::abs(md.deviation_pct);
    if (abs_dev < 0.18) {
        sig.reject_reason = "trend_follow_dev_too_small";
        return sig;
    }

    double ask = side == Side::UP ? quotes.up_ask : quotes.down_ask;
    double bid = side == Side::UP ? quotes.up_bid : quotes.down_bid;
    std::string token = side == Side::UP ? quotes.up_token_id : quotes.down_token_id;
    if (ask < 0.65 || ask > 0.72) {
        sig.reject_reason = "trend_follow_price_window";
        return sig;
    }
    if (bid <= 0 || ask - bid > 0.02) {
        sig.reject_reason = "trend_follow_spread_wide";
        return sig;
    }
    double entry_price = std::max(0.01, ask - 0.01);
    if (entry_price > 0.68) {
        if (abs_dev < 0.22) {
            sig.reject_reason = "trend_follow_high_entry_dev_weak";
            return sig;
        }
        if (ask - bid > 0.01) {
            sig.reject_reason = "trend_follow_high_entry_spread_wide";
            return sig;
        }
    }

    sig.valid = true;
    sig.side = side;
    sig.market_ask = ask;
    sig.entry_price = entry_price;
    sig.size_usdc = 1.00;
    sig.shares = sig.size_usdc / sig.entry_price;
    sig.token_id = token;
    return sig;
}

EntrySignal ExperimentStrategy::evaluate_legacy_cheap_v2(
    const BtcMarketData& md,
    const ExperimentQuotes& quotes,
    const std::string& condition_id,
    const std::string& question) {
    EntrySignal sig;
    sig.condition_id = condition_id;
    sig.market_question = question;
    sig.coin = coin_;
    sig.regime = StrategyRegime::LEGACY_CHEAP;

    if (md.minutes_remaining <= 35) {
        sig.reject_reason = "legacy_v2_time_too_short";
        return sig;
    }

    LegacyV2Filter filter = legacy_v2_filter_for_coin(coin_);
    Side side = Side::NONE;
    double ask = 0;
    double bid = 0;
    std::string token;
    if (quotes.up_ask > 0 && (quotes.down_ask <= 0 || quotes.up_ask <= quotes.down_ask)) {
        side = Side::UP;
        ask = quotes.up_ask;
        bid = quotes.up_bid;
        token = quotes.up_token_id;
    } else if (quotes.down_ask > 0) {
        side = Side::DOWN;
        ask = quotes.down_ask;
        bid = quotes.down_bid;
        token = quotes.down_token_id;
    } else {
        sig.reject_reason = "legacy_v2_no_valid_ask";
        return sig;
    }

    if (ask <= 0 || ask > 0.30 || bid <= 0) {
        sig.reject_reason = "legacy_v2_invalid_price";
        return sig;
    }
    if (std::abs(md.deviation_pct) > filter.max_abs_dev) {
        sig.reject_reason = "legacy_v2_dev_too_large";
        return sig;
    }
    if ((side == Side::UP && md.deviation_pct < -0.12) ||
        (side == Side::DOWN && md.deviation_pct > 0.12)) {
        sig.reject_reason = "legacy_v2_dev_against_side";
        return sig;
    }

    double spread = ask - bid;
    if (spread < 0 || spread > filter.max_spread) {
        sig.reject_reason = "legacy_v2_spread_wide";
        return sig;
    }

    double entry_price = std::max(0.01, ask - 0.01);
    if (entry_price < filter.min_entry || entry_price > filter.max_entry) {
        sig.reject_reason = "legacy_v2_entry_range";
        return sig;
    }
    if (entry_price >= 0.20 && entry_price < 0.25) {
        if (spread > 0.010001) {
            sig.reject_reason = "legacy_v2_mid_spread_wide";
            return sig;
        }
        double mid_dev_limit = filter.max_abs_dev * 0.80;
        if (std::abs(md.deviation_pct) > mid_dev_limit) {
            sig.reject_reason = "legacy_v2_mid_dev_too_large";
            return sig;
        }
    }

    sig.valid = true;
    sig.side = side;
    sig.market_ask = ask;
    sig.entry_price = entry_price;
    sig.size_usdc = 1.00;
    sig.shares = sig.size_usdc / sig.entry_price;
    sig.token_id = token;
    return sig;
}

std::vector<TakeProfitLevel> ExperimentStrategy::compute_tp_levels(
    double entry_price, StrategyRegime regime) const {
    std::vector<TakeProfitLevel> levels;
    if (regime == StrategyRegime::TREND) {
        levels.push_back({0, std::min(0.90, entry_price + 0.08), 0.50, false});
        levels.push_back({1, std::min(0.92, entry_price + 0.15), 1.00, false});
    } else if (regime == StrategyRegime::QUIET_REVERSION) {
        levels.push_back({0, 0.42, 0.50, false});
        levels.push_back({1, 0.62, 1.00, false});
    } else {
        levels.push_back({0, 0.45, 0.50, false});
        levels.push_back({1, 0.70, 0.50, false});
        levels.push_back({2, 0.88, 1.00, false});
    }
    return levels;
}

std::vector<TakeProfitLevel> ExperimentStrategy::compute_trend_follow_tp_levels(
    double entry_price) const {
    std::vector<TakeProfitLevel> levels;
    levels.push_back({0, std::min(0.90, entry_price + 0.10), 0.50, false});
    levels.push_back({1, std::min(0.90, entry_price + 0.15), 0.50, false});
    levels.push_back({2, 0.90, 1.00, false});
    return levels;
}

ExitSignal ExperimentStrategy::evaluate_exit(const Position& pos,
                                             double current_contract_price,
                                             const BtcMarketData& md) const {
    ExitSignal exit;
    int64_t elapsed_sec = (wall_now_ms() - pos.entry_time) / 1000;
    double mfe = pos.max_price - pos.entry_price;

    if (pos.regime == StrategyRegime::TREND) {
        if (current_contract_price <= pos.entry_price - 0.10) {
            exit.should_exit = true; exit.reason = "stop_price"; exit.exit_price = current_contract_price; return exit;
        }
        bool dev_crossed_zero =
            (pos.side == Side::UP && md.deviation_pct <= 0) ||
            (pos.side == Side::DOWN && md.deviation_pct >= 0);
        if (dev_crossed_zero) {
            exit.should_exit = true; exit.reason = "stop_btc"; exit.exit_price = current_contract_price; return exit;
        }
        if (elapsed_sec >= 300 && mfe < 0.03) {
            exit.should_exit = true; exit.reason = "dead_water_exit"; exit.exit_price = current_contract_price; return exit;
        }
        if (mfe >= 0.15) {
            double stop = std::max(pos.entry_price + 0.06, pos.max_price - 0.05);
            if (current_contract_price <= stop) {
                exit.should_exit = true; exit.reason = "trailing_stop"; exit.exit_price = current_contract_price; return exit;
            }
        } else if (mfe >= 0.08) {
            double stop = std::max(pos.entry_price + 0.02, pos.max_price - 0.06);
            if (current_contract_price <= stop) {
                exit.should_exit = true; exit.reason = "trailing_stop"; exit.exit_price = current_contract_price; return exit;
            }
        }
        return exit;
    }

    if (pos.regime == StrategyRegime::QUIET_REVERSION) {
        double abs_dev = std::abs(md.deviation_pct);
        if (current_contract_price <= pos.entry_price - 0.07 + 0.001) {
            exit.should_exit = true; exit.reason = "stop_price"; exit.exit_price = current_contract_price; return exit;
        }
        if (abs_dev > params_.quiet_max_dev + 0.05) {
            exit.should_exit = true; exit.reason = "stop_btc"; exit.exit_price = current_contract_price; return exit;
        }
        if (md.minutes_remaining <= 12 && pos.max_price < 0.42) {
            exit.should_exit = true; exit.reason = "stop_time"; exit.exit_price = current_contract_price; return exit;
        }
        return exit;
    }

    double loss_pct = (pos.entry_price - current_contract_price) / pos.entry_price;
    if (loss_pct >= 0.30) {
        exit.should_exit = true; exit.reason = "stop_price"; exit.exit_price = current_contract_price; return exit;
    }
    double entry_dev = (pos.btc_price_at_entry - pos.btc_strike_at_entry) /
        pos.btc_strike_at_entry * 100.0;
    bool same_dev_side = (entry_dev >= 0 && md.deviation_pct >= 0) ||
                         (entry_dev <= 0 && md.deviation_pct <= 0);
    bool dev_expanded_again = same_dev_side &&
        std::abs(md.deviation_pct) > std::abs(entry_dev) + 0.03;
    if (dev_expanded_again && current_contract_price <= pos.entry_price - 0.07) {
        exit.should_exit = true; exit.reason = "stop_btc"; exit.exit_price = current_contract_price; return exit;
    }
    if (elapsed_sec >= 180 && mfe < 0.02 &&
        current_contract_price <= pos.entry_price - 0.03) {
        exit.should_exit = true; exit.reason = "fast_fail_exit"; exit.exit_price = current_contract_price; return exit;
    }
    bool has_tp = std::any_of(pos.tp_levels.begin(), pos.tp_levels.end(),
        [](const TakeProfitLevel& tp) { return tp.triggered; });
    if (has_tp && current_contract_price <= pos.entry_price + 0.02) {
        exit.should_exit = true; exit.reason = "trailing_stop"; exit.exit_price = current_contract_price; return exit;
    }
    bool shallow_loss = current_contract_price >= pos.entry_price * 0.85;
    if (elapsed_sec >= 480 && mfe < 0.02 && shallow_loss) {
        exit.should_exit = true; exit.reason = "dead_water_exit"; exit.exit_price = current_contract_price; return exit;
    }
    if (mfe >= 0.15) {
        double stop = std::max(pos.max_price - 0.15, pos.entry_price + 0.10);
        if (current_contract_price <= stop) {
            exit.should_exit = true; exit.reason = "trailing_stop"; exit.exit_price = current_contract_price; return exit;
        }
    } else if (mfe >= 0.10) {
        double stop = pos.entry_price + 0.05;
        if (current_contract_price <= stop) {
            exit.should_exit = true; exit.reason = "trailing_stop"; exit.exit_price = current_contract_price; return exit;
        }
    }
    if (md.minutes_remaining <= 10 && current_contract_price < 0.25) {
        exit.should_exit = true; exit.reason = "stop_time"; exit.exit_price = current_contract_price; return exit;
    }
    return exit;
}

ExitSignal ExperimentStrategy::evaluate_trend_follow_exit(
    const Position& pos,
    double current_contract_price,
    const BtcMarketData& md) const {
    ExitSignal exit;
    int64_t elapsed_sec = (wall_now_ms() - pos.entry_time) / 1000;
    double mfe = pos.max_price - pos.entry_price;

    if (current_contract_price <= pos.entry_price - 0.08) {
        exit.should_exit = true;
        exit.reason = "stop_price";
        exit.exit_price = current_contract_price;
        return exit;
    }

    double entry_dev = pos.btc_strike_at_entry > 0
        ? (pos.btc_price_at_entry - pos.btc_strike_at_entry) /
            pos.btc_strike_at_entry * 100.0
        : 0.0;
    bool dev_momentum_faded =
        (pos.side == Side::UP && md.deviation_pct < entry_dev - 0.08) ||
        (pos.side == Side::DOWN && md.deviation_pct > entry_dev + 0.08);
    if (dev_momentum_faded) {
        exit.should_exit = true;
        exit.reason = "stop_btc";
        exit.exit_price = current_contract_price;
        return exit;
    }

    if (elapsed_sec >= 150 && mfe < 0.02 &&
        current_contract_price <= pos.entry_price - 0.03) {
        exit.should_exit = true;
        exit.reason = "fast_fail_exit";
        exit.exit_price = current_contract_price;
        return exit;
    }

    bool has_tp = std::any_of(pos.tp_levels.begin(), pos.tp_levels.end(),
        [](const TakeProfitLevel& tp) { return tp.triggered; });
    if (has_tp && current_contract_price <= pos.entry_price + 0.02) {
        exit.should_exit = true;
        exit.reason = "trailing_stop";
        exit.exit_price = current_contract_price;
        return exit;
    }

    if (elapsed_sec >= 480 && mfe < 0.03) {
        exit.should_exit = true;
        exit.reason = "dead_water_exit";
        exit.exit_price = current_contract_price;
        return exit;
    }

    if (mfe >= 0.12) {
        double stop = std::max(pos.entry_price + 0.05, pos.max_price - 0.04);
        if (current_contract_price <= stop) {
            exit.should_exit = true;
            exit.reason = "trailing_stop";
            exit.exit_price = current_contract_price;
            return exit;
        }
    } else if (mfe >= 0.08) {
        double stop = std::max(pos.entry_price + 0.02, pos.max_price - 0.05);
        if (current_contract_price <= stop) {
            exit.should_exit = true;
            exit.reason = "trailing_stop";
            exit.exit_price = current_contract_price;
            return exit;
        }
    }

    bool dev_still_aligned = trend_follow_side_aligned(pos.side, md.deviation_pct);
    if (md.minutes_remaining <= 5 &&
        (current_contract_price < 0.88 || !dev_still_aligned)) {
        exit.should_exit = true;
        exit.reason = "stop_time";
        exit.exit_price = current_contract_price;
        return exit;
    }

    if (md.minutes_remaining <= 8 &&
        (current_contract_price < 0.78 || !dev_still_aligned)) {
        exit.should_exit = true;
        exit.reason = "stop_time";
        exit.exit_price = current_contract_price;
        return exit;
    }

    return exit;
}

ExperimentEngine::ExperimentEngine(const AppConfig& cfg,
                                   std::string strategy_name,
                                   std::string log_path,
                                   std::string id_prefix)
    : cfg_(cfg), enabled_(cfg.experiment.enabled),
      balance_(cfg.experiment.initial_balance),
      strategy_name_(std::move(strategy_name)),
      log_path_(std::move(log_path)),
      id_prefix_(std::move(id_prefix)) {
    for (const auto& coin : cfg_.coins) {
        strategies_.emplace(coin.coin, ExperimentStrategy(cfg_, coin.coin));
    }
}

void ExperimentEngine::reset_candle(const std::string& coin) {
    std::lock_guard<std::mutex> lock(mu_);
    coin_state_[coin] = CoinRiskState{};
}

void ExperimentEngine::reset_global_hour() {
    std::lock_guard<std::mutex> lock(mu_);
    global_trades_this_hour_ = 0;
    quiet_trades_this_hour_ = 0;
    non_btc_stop_price_this_hour_ = 0;
}

bool ExperimentEngine::has_open_coin(const std::string& coin) const {
    return std::any_of(positions_.begin(), positions_.end(),
        [&](const Position& p) { return !p.closed && p.coin == coin; });
}

void ExperimentEngine::on_market(const std::string& coin,
                                 const BtcMarketData& md,
                                 const MarketEntry& entry,
                                 const ExperimentQuotes& quotes,
                                 int64_t now_ms) {
    if (!enabled_) return;
    if (cfg_.strategy.mode == "live" && coin != "BTC") return;
    std::lock_guard<std::mutex> lock(mu_);
    auto strat_it = strategies_.find(coin);
    if (strat_it == strategies_.end()) return;

    auto close_expired = [&](Position& pos) {
        bool won = (pos.side == Side::UP && md.current_price > pos.btc_strike_at_entry) ||
                   (pos.side == Side::DOWN && md.current_price <= pos.btc_strike_at_entry);
        double settlement_price = won ? 1.0 : 0.0;
        double remaining_shares = pos.shares * pos.shares_remaining_pct;
        double sell_value = remaining_shares * settlement_price;
        double cost_basis = remaining_shares * pos.entry_price;
        double pnl = sell_value - cost_basis;
        pos.current_price = settlement_price;
        pos.max_price = std::max(pos.max_price, settlement_price);
        pos.min_price = std::min(pos.min_price, settlement_price);
        pos.realized_pnl += pnl;
        pos.closed = true;
        pos.close_reason = "expired";
        balance_ += sell_value;

        TradeRecord rec;
        rec.id = pos.id;
        rec.mode = "experiment";
        rec.coin = pos.coin;
        rec.regime = regime_name_local(pos.regime);
        rec.market_question = pos.market_question;
        rec.side = side_name(pos.side);
        rec.entry_time = pos.entry_time;
        rec.exit_time = now_ms;
        rec.minutes_remaining_at_entry = pos.minutes_remaining_at_entry;
        rec.entry_price = pos.entry_price;
        rec.exit_price = settlement_price;
        rec.size_usdc = remaining_shares * pos.entry_price;
        rec.shares = remaining_shares;
        rec.exit_reason = "expired";
        rec.realized_pnl = pnl;
        rec.fee_paid = 0;
        fill_record_analytics(rec, pos, md, now_ms);
        record_trade(rec);
    };

    for (auto& pos : positions_) {
        if (pos.closed || pos.coin != coin) continue;
        bool stale_market = pos.condition_id != entry.market.condition_id;
        if (stale_market || md.minutes_remaining <= 0) {
            close_expired(pos);
            continue;
        }
        double current_price = pos.side == Side::UP ? quotes.up_bid : quotes.down_bid;
        if (current_price <= 0) continue;
        pos.current_price = current_price;
        pos.max_price = std::max(pos.max_price, current_price);
        pos.min_price = std::min(pos.min_price, current_price);
        int64_t elapsed_sec = (now_ms - pos.entry_time) / 1000;
        if (elapsed_sec <= 300 && current_price > pos.mfe_at_5min) {
            pos.mfe_at_5min = current_price;
        }
        if (elapsed_sec <= 600 && current_price > pos.mfe_at_10min) {
            pos.mfe_at_10min = current_price;
        }
        if (elapsed_sec <= 900 && current_price > pos.mfe_at_15min) {
            pos.mfe_at_15min = current_price;
        }

        for (auto& tp : pos.tp_levels) {
            if (tp.triggered || current_price < tp.trigger_price) continue;
            double sell_shares = pos.shares * pos.shares_remaining_pct * tp.sell_pct;
            double sell_value = sell_shares * current_price;
            double cost_basis = sell_shares * pos.entry_price;
            double exit_fee = fee(sell_shares, current_price, true, cfg_.fees);
            double pnl = sell_value - cost_basis - exit_fee;
            pos.shares_remaining_pct = pos.shares > 0
                ? std::max(0.0, (pos.shares * pos.shares_remaining_pct - sell_shares) / pos.shares)
                : 0.0;
            pos.realized_pnl += pnl;
            balance_ += sell_value - exit_fee;
            tp.triggered = true;

            TradeRecord rec;
            rec.id = pos.id + "-TP" + std::to_string(tp.tier);
            rec.mode = "experiment";
            rec.coin = pos.coin;
            rec.regime = regime_name_local(pos.regime);
            rec.market_question = pos.market_question;
            rec.side = side_name(pos.side);
            rec.entry_time = pos.entry_time;
            rec.exit_time = now_ms;
            rec.minutes_remaining_at_entry = pos.minutes_remaining_at_entry;
            rec.entry_price = pos.entry_price;
            rec.exit_price = current_price;
            rec.size_usdc = sell_shares * pos.entry_price;
            rec.shares = sell_shares;
            rec.exit_reason = "tp" + std::to_string(tp.tier);
            rec.realized_pnl = pnl;
            rec.fee_paid = exit_fee;
            fill_record_analytics(rec, pos, md, now_ms);
            record_trade(rec);
        }
        if (pos.shares_remaining_pct < 0.01) {
            pos.closed = true;
            continue;
        }

        auto exit = strategy_name_ == "trend_follow"
            ? strat_it->second.evaluate_trend_follow_exit(pos, current_price, md)
            : strat_it->second.evaluate_exit(pos, current_price, md);
        if (exit.should_exit) {
            double remaining_shares = pos.shares * pos.shares_remaining_pct;
            double sell_value = remaining_shares * exit.exit_price;
            double cost_basis = remaining_shares * pos.entry_price;
            double exit_fee = fee(remaining_shares, exit.exit_price, true, cfg_.fees);
            double pnl = sell_value - cost_basis - exit_fee;
            pos.realized_pnl += pnl;
            pos.closed = true;
            coin_state_[coin].candle_stopped = true;
            if (exit.reason == "stop_price" && coin != "BTC") non_btc_stop_price_this_hour_++;
            balance_ += sell_value - exit_fee;

            TradeRecord rec;
            rec.id = pos.id;
            rec.mode = "experiment";
            rec.coin = pos.coin;
            rec.regime = regime_name_local(pos.regime);
            rec.market_question = pos.market_question;
            rec.side = side_name(pos.side);
            rec.entry_time = pos.entry_time;
            rec.exit_time = now_ms;
            rec.minutes_remaining_at_entry = pos.minutes_remaining_at_entry;
            rec.entry_price = pos.entry_price;
            rec.exit_price = exit.exit_price;
            rec.size_usdc = remaining_shares * pos.entry_price;
            rec.shares = remaining_shares;
            rec.exit_reason = exit.reason;
            rec.realized_pnl = pnl;
            rec.fee_paid = exit_fee;
            fill_record_analytics(rec, pos, md, now_ms);
            record_trade(rec);
        }
    }

    auto& cs = coin_state_[coin];
    if (cs.candle_stopped || cs.candle_traded || has_open_coin(coin)) return;
    if (global_trades_this_hour_ >= cfg_.strategy.max_global_trades_per_hour) return;
    if (coin != "BTC" && non_btc_stop_price_this_hour_ >= 3) {
        reject_counts_["non_btc_stop_price_limit"]++;
        return;
    }

    EntrySignal sig;
    if (strategy_name_ == "trend_follow") {
        sig = strat_it->second.evaluate_trend_follow(md, quotes, entry.market.condition_id,
                                                     entry.market.question);
    } else if (strategy_name_ == "legacy_cheap_v2") {
        sig = strat_it->second.evaluate_legacy_cheap_v2(md, quotes, entry.market.condition_id,
                                                        entry.market.question);
    } else {
        sig = strat_it->second.evaluate_entry(md, quotes, entry.market.condition_id,
                                              entry.market.question);
    }
    if (!sig.valid) {
        if (!sig.reject_reason.empty()) reject_counts_[sig.reject_reason]++;
        return;
    }
    if (sig.regime == StrategyRegime::QUIET_REVERSION &&
        quiet_trades_this_hour_ >= cfg_.strategy.max_quiet_trades_per_hour) {
        reject_counts_["quiet_hour_trades"]++;
        return;
    }
    if (sig.size_usdc > balance_) {
        reject_counts_["insufficient_balance"]++;
        return;
    }

    Position pos;
    pos.id = id_prefix_ + std::to_string(now_ms) + "-" + std::to_string(next_id_++);
    pos.side = sig.side;
    pos.regime = sig.regime;
    pos.coin = sig.coin;
    pos.token_id = sig.token_id;
    pos.condition_id = sig.condition_id;
    pos.market_question = sig.market_question;
    pos.entry_price = sig.entry_price;
    pos.current_price = sig.market_ask;
    pos.size_usdc = sig.size_usdc;
    pos.shares = sig.shares;
    pos.btc_price_at_entry = md.current_price;
    pos.btc_strike_at_entry = md.strike_price;
    pos.entry_vol = md.current_1h_vol;
    pos.avg_vol = md.avg_24h_vol;
    pos.entry_time = now_ms;
    pos.minutes_remaining_at_entry = md.minutes_remaining;
    pos.tp_levels = strategy_name_ == "trend_follow"
        ? strat_it->second.compute_trend_follow_tp_levels(sig.entry_price)
        : strat_it->second.compute_tp_levels(sig.entry_price, sig.regime);
    pos.max_price = sig.entry_price;
    pos.min_price = sig.entry_price;
    pos.mfe_at_5min = sig.entry_price;
    pos.mfe_at_10min = sig.entry_price;
    pos.mfe_at_15min = sig.entry_price;
    pos.spread_at_entry = sig.side == Side::UP
        ? quotes.up_ask - quotes.up_bid : quotes.down_ask - quotes.down_bid;
    positions_.push_back(pos);
    balance_ -= sig.size_usdc;
    cs.candle_traded = true;
    global_trades_this_hour_++;
    if (sig.regime == StrategyRegime::QUIET_REVERSION) quiet_trades_this_hour_++;
    last_signal_ = coin + ":" + regime_name_local(sig.regime) + " " + side_name(sig.side) +
        " @" + std::to_string(sig.entry_price);
}

void ExperimentEngine::prune_closed() {
    std::lock_guard<std::mutex> lock(mu_);
    positions_.erase(std::remove_if(positions_.begin(), positions_.end(),
                     [](const Position& p) { return p.closed; }), positions_.end());
}

void ExperimentEngine::fill_record_analytics(TradeRecord& rec, const Position& pos,
                                             const BtcMarketData& md,
                                             int64_t now_ms) const {
    rec.max_price = pos.max_price;
    rec.min_price = pos.min_price;
    rec.btc_price_at_entry = pos.btc_price_at_entry;
    rec.btc_strike = pos.btc_strike_at_entry;
    rec.btc_deviation_pct = pos.btc_strike_at_entry > 0
        ? (pos.btc_price_at_entry - pos.btc_strike_at_entry) / pos.btc_strike_at_entry * 100.0 : 0.0;
    rec.entry_vol = pos.entry_vol;
    rec.avg_vol = pos.avg_vol;
    rec.btc_price_at_exit = md.current_price;
    rec.btc_deviation_at_exit = md.deviation_pct;
    rec.spread_at_entry = pos.spread_at_entry;
    rec.hold_duration_sec = static_cast<int>((now_ms - pos.entry_time) / 1000);
    double mfe_range = pos.max_price - pos.entry_price;
    rec.mfe_capture_rate = mfe_range > 0 ? (rec.exit_price - pos.entry_price) / mfe_range : 0;
    rec.mfe_at_5min = pos.mfe_at_5min;
    rec.mfe_at_10min = pos.mfe_at_10min;
    rec.mfe_at_15min = pos.mfe_at_15min;
    if (pos.entry_price > 0) {
        rec.mfe5_gain_pct = (pos.mfe_at_5min - pos.entry_price) / pos.entry_price;
        rec.mfe10_gain_pct = (pos.mfe_at_10min - pos.entry_price) / pos.entry_price;
        rec.mfe15_gain_pct = (pos.mfe_at_15min - pos.entry_price) / pos.entry_price;
    }
}

void ExperimentEngine::record_trade(const TradeRecord& rec) {
    trades_.push_back(rec);
    auto& st = regime_stats_[rec.regime.empty() ? "none" : rec.regime];
    st.trades++;
    st.pnl += rec.realized_pnl;
    if (rec.realized_pnl > 0) st.wins++;
    else st.losses++;
    append_jsonl(log_path_, rec);
}

std::string ExperimentEngine::status_json() const {
    std::lock_guard<std::mutex> lock(mu_);
    nlohmann::json j;
    j["enabled"] = enabled_;
    j["strategy"] = strategy_name_;
    j["balance"] = balance_;
    double pnl = 0;
    for (const auto& t : trades_) pnl += t.realized_pnl;
    j["realized_pnl"] = pnl;
    int open_count = 0;
    for (const auto& p : positions_) {
        if (!p.closed) open_count++;
    }
    j["open_positions"] = open_count;
    j["total_trades"] = static_cast<int>(trades_.size());
    j["last_signal"] = last_signal_;
    nlohmann::json pos = nlohmann::json::array();
    for (const auto& p : positions_) {
        if (p.closed) continue;
        nlohmann::json x;
        x["id"] = p.id;
        x["coin"] = p.coin;
        x["regime"] = regime_name_local(p.regime);
        x["side"] = side_name(p.side);
        x["market"] = p.market_question;
        x["entry_price"] = p.entry_price;
        x["current_price"] = p.current_price;
        x["shares"] = p.shares;
        x["remaining_pct"] = p.shares_remaining_pct;
        x["minutes_remaining_at_entry"] = p.minutes_remaining_at_entry;
        x["max_price"] = p.max_price;
        x["min_price"] = p.min_price;
        pos.push_back(x);
    }
    j["positions"] = pos;
    nlohmann::json stats = nlohmann::json::object();
    for (const auto& [k, v] : regime_stats_) {
        stats[k] = {{"trades", v.trades}, {"wins", v.wins}, {"losses", v.losses},
                    {"pnl", v.pnl}, {"win_rate", v.trades > 0 ? v.wins * 100.0 / v.trades : 0.0}};
    }
    j["regime_stats"] = stats;
    nlohmann::json rejects = nlohmann::json::object();
    for (const auto& [k, v] : reject_counts_) rejects[k] = v;
    j["rejects"] = rejects;
    return j.dump();
}

std::string ExperimentEngine::trades_json() const {
    std::lock_guard<std::mutex> lock(mu_);
    nlohmann::json arr = nlohmann::json::array();
    int start = std::max(0, static_cast<int>(trades_.size()) - 200);
    for (int i = start; i < static_cast<int>(trades_.size()); ++i) {
        const auto& t = trades_[i];
        nlohmann::json j;
        j["id"] = t.id;
        j["mode"] = t.mode;
        j["coin"] = t.coin;
        j["regime"] = t.regime;
        j["market"] = t.market_question;
        j["side"] = t.side;
        j["entry_time"] = t.entry_time;
        j["exit_time"] = t.exit_time;
        j["entry_price"] = t.entry_price;
        j["exit_price"] = t.exit_price;
        j["shares"] = t.shares;
        j["size_usdc"] = t.size_usdc;
        j["exit_reason"] = t.exit_reason;
        j["pnl"] = t.realized_pnl;
        arr.push_back(j);
    }
    return arr.dump();
}

std::string ExperimentEngine::all_trades_json() const {
    nlohmann::json arr = nlohmann::json::array();
    std::ifstream in(log_path_);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        try {
            arr.push_back(nlohmann::json::parse(line));
        } catch (...) {
        }
    }
    return arr.dump();
}

}  // namespace polymarket
