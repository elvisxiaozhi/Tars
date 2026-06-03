#include "core/experiment_engine.h"

#include "core/build_info.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <fstream>
#include <sstream>

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
        return {0.20, 0.24, 0.12, 0.03};
    }
    if (coin == "ETH") {
        return {0.20, 0.24, 0.12, 0.02};
    }
    return {0.20, 0.24, 0.20, 0.02};
}

bool trend_follow_side_aligned(Side side, double deviation_pct) {
    return (side == Side::UP && deviation_pct > 0) ||
           (side == Side::DOWN && deviation_pct < 0);
}

std::string lower_copy_local(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool finance_asset_allowed(const std::string& asset) {
    return asset == "SPX" || asset == "GOLD";
}

double finance_dev_threshold(const std::string& asset) {
    if (asset == "SPY" || asset == "SPX") return 0.35;
    if (asset == "GOLD") return 0.45;
    if (asset == "WTI") return 0.80;
    return 0.80;
}

double finance_extreme_dev_threshold(const std::string& asset) {
    if (asset == "SPY" || asset == "SPX") return 1.00;
    if (asset == "GOLD") return 1.20;
    if (asset == "WTI") return 2.00;
    return 2.00;
}

double finance_max_sane_dev(const std::string& asset) {
    if (asset == "SPY" || asset == "SPX") return 5.00;
    if (asset == "GOLD") return 5.00;
    if (asset == "WTI") return 10.00;
    return 5.00;
}

bool crypto_duration_asset_allowed(const std::string& coin, bool daily) {
    if (daily) return coin == "BTC" || coin == "ETH" || coin == "SOL" || coin == "BNB";
    return coin == "BTC" || coin == "ETH" || coin == "SOL" || coin == "BNB" || coin == "XRP";
}

double crypto_duration_dev_threshold(const std::string& coin, bool daily) {
    if (daily) {
        if (coin == "BTC" || coin == "ETH") return 0.60;
        return 0.90;
    }
    if (coin == "BTC" || coin == "ETH") return 0.35;
    return 0.50;
}

std::string trend_alignment(Side side, const BtcMarketData* md) {
    if (!md) return "missing";
    if (std::abs(md->deviation_pct) < 0.05) return "neutral";
    if (trend_follow_side_aligned(side, md->deviation_pct)) return "aligned";
    return "opposed";
}

std::string trend_price_bucket(double entry_price) {
    if (entry_price <= 0.66) return "<=0.66";
    if (entry_price <= 0.68) return "0.66-0.68";
    if (entry_price <= 0.71) return "0.68-0.71";
    return ">0.71";
}

std::string cheap_price_bucket(double entry_price) {
    if (entry_price < 0.20) return "<0.20";
    if (entry_price < 0.25) return "0.20-0.24";
    if (entry_price < 0.27) return "0.25-0.26";
    return ">=0.27";
}

bool opposed_pair(const BtcMarketData* a, const BtcMarketData* b) {
    if (!a || !b) return false;
    if (std::abs(a->deviation_pct) < 0.05 || std::abs(b->deviation_pct) < 0.05) return false;
    return (a->deviation_pct > 0 && b->deviation_pct < 0) ||
           (a->deviation_pct < 0 && b->deviation_pct > 0);
}

std::string cross_coin_state(const std::string& btc_alignment,
                             const std::string& eth_alignment,
                             bool btc_eth_opposed) {
    if (btc_eth_opposed) return "btc_eth_diverged";
    if (btc_alignment == "opposed" || eth_alignment == "opposed") return "mixed";
    if (btc_alignment == "missing" && eth_alignment == "missing") return "missing";
    return "aligned";
}

std::string cheap_background_alignment(Side side, const BtcMarketData* md) {
    if (!md) return "missing";
    if (std::abs(md->deviation_pct) < 0.05) return "neutral";
    if ((side == Side::UP && md->deviation_pct > -0.05) ||
        (side == Side::DOWN && md->deviation_pct < 0.05)) {
        return "supportive";
    }
    if ((side == Side::UP && md->deviation_pct < -0.12) ||
        (side == Side::DOWN && md->deviation_pct > 0.12)) {
        return "opposed";
    }
    return "mixed";
}

// taker 费率由调用方按品类传入（Crypto 0.07 / Finance 0.04）；maker 免费故入场不计费。
double fee(double shares, double price, double rate, const FeeConfig& fees) {
    return shares * rate * price * (1.0 - price) + fees.gas_per_tx_usdc;
}

void append_jsonl(const std::string& path, const TradeRecord& rec) {
    nlohmann::json j;
    j["id"] = rec.id;
    j["mode"] = rec.mode;
    j["code_version"] = code_version();
    j["config_hash"] = config_hash();
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
    j["armed_at_ms"] = rec.armed_at_ms;
    j["min_price_after_arm"] = rec.min_price_after_arm;
    j["strategy"] = rec.strategy;
    j["entry_confidence"] = rec.entry_confidence;
    j["btc_alignment"] = rec.btc_alignment;
    j["eth_alignment"] = rec.eth_alignment;
    j["cross_coin_state"] = rec.cross_coin_state;
    j["entry_price_bucket"] = rec.entry_price_bucket;
    j["confidence_components"] = rec.confidence_components;
    j["max_favorable"] = rec.max_favorable;
    j["max_adverse"] = rec.max_adverse;
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
    const std::string& question,
    const TrendFollowContext& ctx) {
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

    if (coin_ != "BTC" && coin_ != "ETH" && coin_ != "BNB") {
        sig.reject_reason = "trend_follow_coin_filter";
        return sig;
    }

    if (md.minutes_remaining < 41 || md.minutes_remaining > 45) {
        sig.reject_reason = "trend_follow_time_window";
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
    if (ask < 0.65 || ask > 0.68) {
        sig.reject_reason = "trend_follow_price_window";
        return sig;
    }
    if (bid <= 0 || ask - bid > 0.010001) {
        sig.reject_reason = "trend_follow_spread_wide";
        return sig;
    }
    double entry_price = std::max(0.01, ask - 0.01);
    if (entry_price < 0.64 || entry_price > 0.67) {
        sig.reject_reason = "trend_follow_entry_range";
        return sig;
    }

    const BtcMarketData* btc_md = ctx.has_btc ? &ctx.btc : nullptr;
    const BtcMarketData* eth_md = ctx.has_eth ? &ctx.eth : nullptr;
    std::string btc_align = trend_alignment(side, btc_md);
    std::string eth_align = trend_alignment(side, eth_md);
    bool btc_eth_diverged = opposed_pair(btc_md, eth_md);
    std::string cross_state = cross_coin_state(btc_align, eth_align, btc_eth_diverged);

    int confidence = 0;
    std::vector<std::string> components;
    confidence += 1;
    components.push_back("target_direction=+1");
    confidence += 1;
    components.push_back("direction_confirmed=+1");
    if (btc_align == "aligned" || btc_align == "neutral") {
        confidence += 1;
        components.push_back("btc_" + btc_align + "=+1");
    }
    if (eth_align == "aligned" || eth_align == "neutral") {
        confidence += 1;
        components.push_back("eth_" + eth_align + "=+1");
    }
    if (abs_dev >= 0.22) {
        confidence += 1;
        components.push_back("target_dev_strong=+1");
    }
    if (entry_price <= 0.68) {
        confidence += 1;
        components.push_back("entry_value=+1");
    }
    if (entry_price > 0.69) {
        confidence -= 1;
        components.push_back("chase_penalty=-1");
    }
    if (btc_eth_diverged) {
        confidence -= 1;
        components.push_back("btc_eth_diverged=-1");
    }
    if (btc_align == "opposed" && btc_md && std::abs(btc_md->deviation_pct) >= 0.12) {
        confidence -= 1;
        components.push_back("btc_strong_opposed=-1");
    }

    if (confidence < 4) {
        sig.reject_reason = "trend_follow_confidence_low";
        return sig;
    }

    sig.valid = true;
    sig.side = side;
    sig.market_ask = ask;
    sig.entry_price = entry_price;
    sig.size_usdc = side == Side::DOWN ? 1.25 : 1.00;
    sig.shares = sig.size_usdc / sig.entry_price;
    sig.token_id = token;
    sig.entry_confidence = confidence;
    sig.btc_alignment = btc_align;
    sig.eth_alignment = eth_align;
    sig.cross_coin_state = cross_state;
    sig.entry_price_bucket = trend_price_bucket(entry_price);
    std::ostringstream component_out;
    for (size_t i = 0; i < components.size(); ++i) {
        if (i > 0) component_out << ",";
        component_out << components[i];
    }
    sig.confidence_components = component_out.str();
    return sig;
}

// trend_v3：trend_v2 基底（BTC、mr40-45、1-tick 方向确认、便宜带、复用 trend exit/TP）
// + 核心新增「dev-accel 门」。设计与回测依据见 docs/steps/step-trend-v3-shadow.md。
// 入场带分层：0.64-0.67 免门（已验证 edge）；0.68-0.79 必须过 dev-accel 门
// （~90s 内 abs_dev 上升 >0.01）。回测：dev 上升 63% vs 不上升 35%；门控高价 64%/+EV。
// 纯 shadow，不影响 main / trend_v2。
EntrySignal ExperimentStrategy::evaluate_trend_v3(
    const BtcMarketData& md,
    const ExperimentQuotes& quotes,
    const std::string& condition_id,
    const std::string& question,
    const TrendFollowContext& /*ctx*/,
    int64_t now_ms) {
    EntrySignal sig;
    sig.condition_id = condition_id;
    sig.market_question = question;
    sig.coin = coin_;
    sig.regime = StrategyRegime::TREND;

    // 换市场重置 per-market 状态
    if (condition_id != last_condition_id_) {
        last_condition_id_ = condition_id;
        trend_prev_side_ = Side::NONE;
        has_trend_prev_side_ = false;
        tv3_dev_hist_.clear();
    }

    double abs_dev = std::abs(md.deviation_pct);
    // 每 poll 累积 dev 历史，修剪到 ~95s 窗口（dev-accel 门用）。在所有过滤之前累积，
    // 保证入场判定时已攒够历史。
    tv3_dev_hist_.emplace_back(now_ms, abs_dev);
    while (tv3_dev_hist_.size() > 1 && now_ms - tv3_dev_hist_.front().first > 95000) {
        tv3_dev_hist_.pop_front();
    }

    if (coin_ != "BTC") {
        sig.reject_reason = "trend_v3_coin_filter";
        return sig;
    }
    if (md.minutes_remaining < 40 || md.minutes_remaining > 45) {
        sig.reject_reason = "trend_v3_time_window";
        return sig;
    }

    Side side = md.deviation_pct > 0 ? Side::UP :
                md.deviation_pct < 0 ? Side::DOWN : Side::NONE;
    if (side == Side::NONE) {
        sig.reject_reason = "no_direction";
        return sig;
    }

    // 1-tick 方向确认（承自 trend_v2）：上一 poll 同向，防假突破
    bool direction_confirmed = has_trend_prev_side_ && trend_prev_side_ == side;
    trend_prev_side_ = side;
    has_trend_prev_side_ = true;
    if (!direction_confirmed) {
        sig.reject_reason = "trend_v3_direction_not_confirmed";
        return sig;
    }

    if (abs_dev < 0.20) {
        sig.reject_reason = "trend_v3_dev_too_small";
        return sig;
    }

    double ask = side == Side::UP ? quotes.up_ask : quotes.down_ask;
    double bid = side == Side::UP ? quotes.up_bid : quotes.down_bid;
    std::string token = side == Side::UP ? quotes.up_token_id : quotes.down_token_id;
    if (bid <= 0 || ask <= 0) {
        sig.reject_reason = "trend_v3_invalid_quote";
        return sig;
    }
    if (ask - bid > 0.010001) {
        sig.reject_reason = "trend_v3_spread_wide";
        return sig;
    }

    double entry_price = std::max(0.01, ask - 0.01);
    if (entry_price < 0.64 || entry_price > 0.79 + 1e-9) {
        sig.reject_reason = "trend_v3_entry_range";
        return sig;
    }

    // dev-accel 门：~90s 窗口内 abs_dev 上升（>0.01）。仅对高价（>0.67）强制要求。
    // 便宜带 0.64-0.67 免门（已验证 edge，且回测里便宜单基本都自带上升）。
    bool have_window = tv3_dev_hist_.size() >= 3 &&
                       now_ms - tv3_dev_hist_.front().first >= 50000;
    bool dev_rising = have_window &&
                      (abs_dev - tv3_dev_hist_.front().second) > 0.01;
    bool high_entry = entry_price > 0.67 + 1e-9;
    if (high_entry) {
        if (!have_window) {
            sig.reject_reason = "trend_v3_accel_warmup";
            return sig;
        }
        if (!dev_rising) {
            sig.reject_reason = "trend_v3_high_entry_no_accel";
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
    sig.entry_price_bucket = high_entry ? "high_gated_0.68_0.79" : "cheap_0.64_0.67";
    sig.confidence_components =
        std::string(high_entry ? "high_entry" : "cheap_entry") +
        (have_window ? (dev_rising ? ",dev_rising" : ",dev_flat") : ",accel_warmup");
    spdlog::info("SIGNAL [{} trend_v3]: {} {} @ {:.3f} (ask={:.3f}, dev={:+.2f}%, {})",
                 coin_, side == Side::UP ? "UP" : "DOWN", question,
                 entry_price, ask, md.deviation_pct, sig.entry_price_bucket);
    return sig;
}

EntrySignal ExperimentStrategy::evaluate_legacy_cheap_v2(
    const BtcMarketData& md,
    const ExperimentQuotes& quotes,
    const std::string& condition_id,
    const std::string& question,
    const TrendFollowContext& ctx) {
    EntrySignal sig;
    sig.condition_id = condition_id;
    sig.market_question = question;
    sig.coin = coin_;
    sig.regime = StrategyRegime::LEGACY_CHEAP;

    if (coin_ != "BTC" && coin_ != "ETH") {
        sig.reject_reason = "legacy_v2_coin_filter";
        return sig;
    }

    if (md.minutes_remaining < 41 || md.minutes_remaining > 45) {
        sig.reject_reason = "legacy_v2_time_window";
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
    double abs_dev = std::abs(md.deviation_pct);
    if (abs_dev < 0.10 || abs_dev > filter.max_abs_dev) {
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
    if (spread > 0.010001) {
        sig.reject_reason = "legacy_v2_spread_not_tight";
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
    }

    const BtcMarketData* btc_md = ctx.has_btc ? &ctx.btc : nullptr;
    const BtcMarketData* eth_md = ctx.has_eth ? &ctx.eth : nullptr;
    std::string btc_align = cheap_background_alignment(side, btc_md);
    std::string eth_align = cheap_background_alignment(side, eth_md);
    bool btc_eth_diverged = opposed_pair(btc_md, eth_md);

    int confidence = 0;
    std::ostringstream components;
    auto add_component = [&](const std::string& name, int delta) {
        confidence += delta;
        if (components.tellp() > 0) components << ",";
        components << (delta > 0 ? "+" : "") << delta << ":" << name;
    };

    if (md.minutes_remaining >= 41 && md.minutes_remaining <= 45) {
        add_component("time_41_45", 1);
    } else {
        add_component("time_outside_41_45", -1);
    }
    add_component("spread_tight", 1);
    if (entry_price >= 0.20 && entry_price < 0.25) {
        add_component("entry_value_20_24", 1);
    }
    if (coin_ == "BTC" || coin_ == "ETH" || coin_ == "SOL") {
        add_component("core_coin", 1);
    } else {
        add_component("non_core_coin", 0);
    }
    if (abs_dev >= 0.10 && abs_dev <= 0.12) {
        add_component("dev_active_10_12", 1);
    }
    if (entry_price >= 0.27) {
        add_component("entry_high_27_plus", -1);
    }
    if (btc_align == "opposed" || eth_align == "opposed") {
        add_component("background_opposed", -1);
    }
    if (btc_eth_diverged) {
        add_component("btc_eth_diverged", -1);
    }

    if (confidence < 4) {
        sig.reject_reason = "legacy_v2_confidence_low";
        return sig;
    }
    if (entry_price >= 0.25 && confidence < 5) {
        sig.reject_reason = "legacy_v2_high_entry_confidence_low";
        return sig;
    }
    if (side == Side::UP && confidence < 5) {
        sig.reject_reason = "legacy_v2_up_confidence_low";
        return sig;
    }

    sig.valid = true;
    sig.side = side;
    sig.market_ask = ask;
    sig.entry_price = entry_price;
    sig.size_usdc = 1.00;
    sig.shares = sig.size_usdc / sig.entry_price;
    sig.token_id = token;
    sig.entry_confidence = confidence;
    sig.btc_alignment = btc_align;
    sig.eth_alignment = eth_align;
    sig.cross_coin_state = cross_coin_state(btc_align, eth_align, btc_eth_diverged);
    sig.entry_price_bucket = cheap_price_bucket(entry_price);
    sig.confidence_components = components.str();
    return sig;
}

// 【已退役 2026-05-30】逆势 cheap-value 失败族第 3 例（全量 202 仓净 −$11.84/37% 胜率），
// 与已退役的 eth_late_cheap_v1 / QUIET_REVERSION 同族同结构。main.cpp 不再实例化。
// 代码保留仅作回放参考，不要重新上线。详见 docs/steps/step-retire-eth-cheap-v1.md。
EntrySignal ExperimentStrategy::evaluate_eth_cheap_v1(
    const BtcMarketData& md,
    const ExperimentQuotes& quotes,
    const std::string& condition_id,
    const std::string& question) {
    EntrySignal sig;
    sig.condition_id = condition_id;
    sig.market_question = question;
    sig.coin = coin_;
    sig.regime = StrategyRegime::QUIET_REVERSION;

    if (coin_ != "ETH") {
        sig.reject_reason = "eth_cheap_coin_filter";
        return sig;
    }
    if (md.minutes_remaining <= 30) {
        sig.reject_reason = "eth_cheap_time_window";
        return sig;
    }

    double abs_dev = std::abs(md.deviation_pct);
    if (abs_dev < 0.10 || abs_dev > 0.24) {
        sig.reject_reason = "eth_cheap_dev_range";
        return sig;
    }

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
        sig.reject_reason = "eth_cheap_no_valid_ask";
        return sig;
    }

    if (bid <= 0 || ask <= 0 || ask - bid > 0.010001) {
        sig.reject_reason = "eth_cheap_spread_wide";
        return sig;
    }

    double entry_price = std::max(0.01, ask - 0.01);
    if (entry_price < 0.20 || entry_price > 0.26) {
        sig.reject_reason = "eth_cheap_entry_range";
        return sig;
    }

    bool medium_value = false;
    if (entry_price <= 0.240001) {
        if (abs_dev > 0.24) {
            sig.reject_reason = "eth_cheap_dev_range";
            return sig;
        }
    } else {
        medium_value = true;
        if (abs_dev > 0.18) {
            sig.reject_reason = "eth_cheap_dev_range";
            return sig;
        }
    }

    sig.valid = true;
    sig.side = side;
    sig.market_ask = ask;
    sig.entry_price = entry_price;
    sig.size_usdc = medium_value ? 0.50 : 1.00;
    sig.shares = sig.size_usdc / sig.entry_price;
    sig.token_id = token;
    sig.entry_price_bucket = cheap_price_bucket(entry_price);
    sig.confidence_components = medium_value
        ? "eth_cheap_v1,medium_entry_25_26,half_size"
        : "eth_cheap_v1,cheap_side,entry_20_24,full_size";
    return sig;
}

EntrySignal ExperimentStrategy::evaluate_eth_only_v1(
    const BtcMarketData& md,
    const ExperimentQuotes& quotes,
    const std::string& condition_id,
    const std::string& question) {
    EntrySignal sig;
    sig.condition_id = condition_id;
    sig.market_question = question;
    sig.coin = coin_;

    if (coin_ != "ETH") {
        sig.reject_reason = "eth_only_coin_filter";
        return sig;
    }
    if (md.minutes_remaining <= 30) {
        sig.reject_reason = "eth_only_time_window";
        return sig;
    }

    double abs_dev = std::abs(md.deviation_pct);

    auto fill_signal = [&](Side side, StrategyRegime regime, double bid, double ask,
                           const std::string& token, double size_usdc) {
        sig.valid = true;
        sig.side = side;
        sig.regime = regime;
        sig.market_ask = ask;
        sig.entry_price = std::max(0.01, ask - 0.01);
        sig.size_usdc = size_usdc;
        sig.shares = sig.size_usdc / sig.entry_price;
        sig.token_id = token;
        sig.entry_price_bucket = regime == StrategyRegime::TREND
            ? trend_price_bucket(sig.entry_price)
            : cheap_price_bucket(sig.entry_price);
    };

    if (abs_dev >= 0.18) {
        Side side = md.deviation_pct > 0 ? Side::UP : Side::DOWN;
        double ask = side == Side::UP ? quotes.up_ask : quotes.down_ask;
        double bid = side == Side::UP ? quotes.up_bid : quotes.down_bid;
        const std::string& token = side == Side::UP ? quotes.up_token_id : quotes.down_token_id;
        if (bid <= 0 || ask <= 0 || ask - bid > 0.010001) {
            sig.reject_reason = "eth_only_trend_spread_wide";
            return sig;
        }
        double entry_price = std::max(0.01, ask - 0.01);
        if (entry_price >= 0.64 && entry_price <= 0.67) {
            fill_signal(side, StrategyRegime::TREND, bid, ask, token, 1.00);
            sig.confidence_components = "eth_trend,entry_64_67,dev_18_plus";
            return sig;
        }
    }

    if (abs_dev < 0.10 || abs_dev > 0.18) {
        sig.reject_reason = "eth_only_cheap_dev_range";
        return sig;
    }

    Side cheap_side = Side::NONE;
    double cheap_ask = 0;
    double cheap_bid = 0;
    std::string cheap_token;
    if (quotes.up_ask > 0 && (quotes.down_ask <= 0 || quotes.up_ask <= quotes.down_ask)) {
        cheap_side = Side::UP;
        cheap_ask = quotes.up_ask;
        cheap_bid = quotes.up_bid;
        cheap_token = quotes.up_token_id;
    } else if (quotes.down_ask > 0) {
        cheap_side = Side::DOWN;
        cheap_ask = quotes.down_ask;
        cheap_bid = quotes.down_bid;
        cheap_token = quotes.down_token_id;
    } else {
        sig.reject_reason = "eth_only_cheap_no_valid_ask";
        return sig;
    }
    if (cheap_bid <= 0 || cheap_ask <= 0 || cheap_ask - cheap_bid > 0.010001) {
        sig.reject_reason = "eth_only_cheap_spread_wide";
        return sig;
    }
    double cheap_entry = std::max(0.01, cheap_ask - 0.01);
    if (cheap_entry < 0.24 || cheap_entry > 0.26) {
        sig.reject_reason = "eth_only_cheap_entry_range";
        return sig;
    }

    fill_signal(cheap_side, StrategyRegime::QUIET_REVERSION,
                cheap_bid, cheap_ask, cheap_token, 1.00);
    sig.confidence_components = "eth_cheap_repricing,cheap_side,entry_24_26,dev_10_plus";
    return sig;
}

EntrySignal ExperimentStrategy::evaluate_late_window_v1(
    const BtcMarketData& md,
    const ExperimentQuotes& quotes,
    const std::string& condition_id,
    const std::string& question) {
    EntrySignal sig;
    sig.condition_id = condition_id;
    sig.market_question = question;
    sig.coin = coin_;
    sig.regime = StrategyRegime::TREND;

    if (coin_ != "BTC" && coin_ != "ETH") {
        sig.reject_reason = "late_coin_filter";
        return sig;
    }
    if (md.minutes_remaining < 10 || md.minutes_remaining > 20) {
        sig.reject_reason = "late_time_window";
        return sig;
    }

    double abs_dev = std::abs(md.deviation_pct);
    if (abs_dev < 0.25) {
        sig.reject_reason = "late_dev_weak";
        return sig;
    }

    Side side = md.deviation_pct > 0 ? Side::UP : Side::DOWN;
    double ask = side == Side::UP ? quotes.up_ask : quotes.down_ask;
    double bid = side == Side::UP ? quotes.up_bid : quotes.down_bid;
    const std::string& token = side == Side::UP ? quotes.up_token_id : quotes.down_token_id;
    if (bid <= 0 || ask <= 0 || ask - bid > 0.010001) {
        sig.reject_reason = "late_spread_wide";
        return sig;
    }

    double entry_price = std::max(0.01, ask - 0.01);
    if (entry_price < 0.35 || entry_price > 0.65) {
        sig.reject_reason = "late_entry_range";
        return sig;
    }

    sig.valid = true;
    sig.side = side;
    sig.market_ask = ask;
    sig.entry_price = entry_price;
    sig.size_usdc = 1.00;
    sig.shares = sig.size_usdc / sig.entry_price;
    sig.token_id = token;
    sig.entry_price_bucket = trend_price_bucket(entry_price);
    sig.confidence_components = "late_window_v1,btc_eth,minutes_10_20,entry_35_65,dev_25_plus";
    return sig;
}

EntrySignal ExperimentStrategy::evaluate_eth_late_cheap_v1(
    const BtcMarketData& md,
    const ExperimentQuotes& quotes,
    const std::string& condition_id,
    const std::string& question) {
    EntrySignal sig;
    sig.condition_id = condition_id;
    sig.market_question = question;
    sig.coin = coin_;
    sig.regime = StrategyRegime::QUIET_REVERSION;

    if (coin_ != "ETH") {
        sig.reject_reason = "eth_late_coin_filter";
        return sig;
    }
    if (md.minutes_remaining < 10 || md.minutes_remaining > 25) {
        sig.reject_reason = "eth_late_time_window";
        return sig;
    }

    double abs_dev = std::abs(md.deviation_pct);
    if (abs_dev < 0.12 || abs_dev > 0.28) {
        sig.reject_reason = "eth_late_dev_range";
        return sig;
    }

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
        sig.reject_reason = "eth_late_no_valid_ask";
        return sig;
    }

    if (bid <= 0 || ask <= 0 || ask - bid > 0.010001) {
        sig.reject_reason = "eth_late_spread_wide";
        return sig;
    }

    double entry_price = std::max(0.01, ask - 0.01);
    if (entry_price < 0.20 || entry_price > 0.28) {
        sig.reject_reason = "eth_late_entry_range";
        return sig;
    }

    sig.valid = true;
    sig.side = side;
    sig.market_ask = ask;
    sig.entry_price = entry_price;
    sig.size_usdc = 0.25;
    sig.shares = sig.size_usdc / sig.entry_price;
    sig.token_id = token;
    sig.entry_price_bucket = cheap_price_bucket(entry_price);
    sig.confidence_components = "eth_late_cheap_v1,minutes_10_25,entry_20_28,quarter_size";
    return sig;
}

EntrySignal ExperimentStrategy::evaluate_finance_updown_v1(
    const BtcMarketData& md,
    const ExperimentQuotes& quotes,
    const std::string& condition_id,
    const std::string& question) {
    EntrySignal sig;
    sig.condition_id = condition_id;
    sig.market_question = question;
    sig.coin = coin_;
    sig.regime = StrategyRegime::TREND;

    if (!finance_asset_allowed(coin_)) {
        sig.reject_reason = "finance_asset_filter";
        return sig;
    }
    if (md.minutes_remaining <= 20 || md.minutes_remaining > 390) {
        sig.reject_reason = "finance_time_window";
        return sig;
    }

    std::string text = lower_copy_local(question);
    if (text.find("up or down") == std::string::npos ||
        text.find("above") != std::string::npos ||
        text.find("below") != std::string::npos ||
        text.find("over") != std::string::npos ||
        text.find("under") != std::string::npos) {
        sig.reject_reason = "finance_not_updown";
        return sig;
    }

    double abs_dev = std::abs(md.deviation_pct);
    if (abs_dev > finance_max_sane_dev(coin_)) {
        sig.reject_reason = "finance_reference_unstable";
        return sig;
    }
    double min_dev = finance_dev_threshold(coin_);
    if (abs_dev < min_dev) {
        sig.reject_reason = "finance_dev_weak";
        return sig;
    }

    auto build = [&](Side side, StrategyRegime regime, double size_usdc,
                     const std::string& regime_label) {
        double ask = side == Side::UP ? quotes.up_ask : quotes.down_ask;
        double bid = side == Side::UP ? quotes.up_bid : quotes.down_bid;
        std::string token = side == Side::UP ? quotes.up_token_id : quotes.down_token_id;
        sig.valid = true;
        sig.side = side;
        sig.regime = regime;
        sig.market_ask = ask;
        sig.entry_price = std::max(0.01, ask - 0.01);
        sig.size_usdc = size_usdc;
        sig.shares = sig.size_usdc / sig.entry_price;
        sig.token_id = token;
        sig.entry_price_bucket = regime == StrategyRegime::TREND
            ? trend_price_bucket(sig.entry_price) : cheap_price_bucket(sig.entry_price);
        std::ostringstream components;
        components << "finance_updown_v1," << coin_ << "," << regime_label
                   << ",dev=" << (abs_dev >= min_dev * 1.5 ? "strong" : "active")
                   << ",entry=" << sig.entry_price_bucket;
        sig.confidence_components = components.str();
    };

    Side trend_side = md.deviation_pct > 0 ? Side::UP : Side::DOWN;
    double trend_ask = trend_side == Side::UP ? quotes.up_ask : quotes.down_ask;
    double trend_bid = trend_side == Side::UP ? quotes.up_bid : quotes.down_bid;
    if (trend_bid <= 0 || trend_ask <= 0 || trend_ask - trend_bid > 0.020001) {
        sig.reject_reason = "finance_spread_wide";
        return sig;
    }

    double trend_entry = std::max(0.01, trend_ask - 0.01);
    if (md.minutes_remaining >= 60 && md.minutes_remaining <= 330 &&
        trend_entry >= 0.55 && trend_entry <= 0.72) {
        build(trend_side, StrategyRegime::TREND, 0.50, "trend");
        return sig;
    }

    Side reversal_side = trend_side == Side::UP ? Side::DOWN : Side::UP;
    double reversal_ask = reversal_side == Side::UP ? quotes.up_ask : quotes.down_ask;
    double reversal_bid = reversal_side == Side::UP ? quotes.up_bid : quotes.down_bid;
    double reversal_entry = std::max(0.01, reversal_ask - 0.01);
    if (md.minutes_remaining >= 90 && md.minutes_remaining <= 300 &&
        abs_dev >= finance_extreme_dev_threshold(coin_) &&
        reversal_bid > 0 && reversal_ask > 0 && reversal_ask - reversal_bid <= 0.020001 &&
        reversal_entry >= 0.18 && reversal_entry <= 0.30) {
        build(reversal_side, StrategyRegime::REVERSAL, 0.25, "extreme_reversal");
        return sig;
    }

    if (trend_entry < 0.55 || trend_entry > 0.72) {
        sig.reject_reason = "finance_trend_entry_range";
        return sig;
    }
    sig.reject_reason = "finance_reversal_filter";
    return sig;
}

EntrySignal ExperimentStrategy::evaluate_crypto_duration_updown_v1(
    const BtcMarketData& md,
    const ExperimentQuotes& quotes,
    const std::string& condition_id,
    const std::string& question,
    bool daily) {
    EntrySignal sig;
    sig.condition_id = condition_id;
    sig.market_question = question;
    sig.coin = coin_;
    sig.regime = StrategyRegime::TREND;

    if (!crypto_duration_asset_allowed(coin_, daily)) {
        sig.reject_reason = daily ? "crypto_daily_coin_filter" : "crypto_4h_coin_filter";
        return sig;
    }
    int min_minutes = daily ? 180 : 60;
    int max_minutes = daily ? 900 : 210;
    if (md.minutes_remaining < min_minutes || md.minutes_remaining > max_minutes) {
        sig.reject_reason = daily ? "crypto_daily_time_window" : "crypto_4h_time_window";
        return sig;
    }

    double abs_dev = std::abs(md.deviation_pct);
    double min_dev = crypto_duration_dev_threshold(coin_, daily);
    if (abs_dev < min_dev) {
        sig.reject_reason = daily ? "crypto_daily_dev_weak" : "crypto_4h_dev_weak";
        return sig;
    }

    Side side = md.deviation_pct > 0 ? Side::UP : Side::DOWN;
    double ask = side == Side::UP ? quotes.up_ask : quotes.down_ask;
    double bid = side == Side::UP ? quotes.up_bid : quotes.down_bid;
    const std::string& token = side == Side::UP ? quotes.up_token_id : quotes.down_token_id;
    double max_spread = daily ? 0.025001 : 0.020001;
    if (bid <= 0 || ask <= 0 || ask - bid > max_spread) {
        sig.reject_reason = daily ? "crypto_daily_spread_wide" : "crypto_4h_spread_wide";
        return sig;
    }

    double entry_price = std::max(0.01, ask - 0.01);
    double min_entry = daily ? 0.58 : 0.55;
    double max_entry = daily ? 0.72 : 0.70;
    if (entry_price < min_entry || entry_price > max_entry) {
        sig.reject_reason = daily ? "crypto_daily_entry_range" : "crypto_4h_entry_range";
        return sig;
    }

    sig.valid = true;
    sig.side = side;
    sig.market_ask = ask;
    sig.entry_price = entry_price;
    sig.size_usdc = daily ? 0.50 : 0.50;
    sig.shares = sig.size_usdc / sig.entry_price;
    sig.token_id = token;
    sig.entry_price_bucket = trend_price_bucket(entry_price);
    std::ostringstream components;
    components << (daily ? "crypto_daily_updown_v1" : "crypto_4h_updown_v1")
               << "," << coin_ << ",trend,dev="
               << (abs_dev >= min_dev * 1.5 ? "strong" : "active")
               << ",entry=" << sig.entry_price_bucket;
    sig.confidence_components = components.str();
    return sig;
}

std::vector<TakeProfitLevel> ExperimentStrategy::compute_tp_levels(
    double entry_price, StrategyRegime regime) const {
    std::vector<TakeProfitLevel> levels;
    if (regime == StrategyRegime::TREND) {
        levels.push_back({0, std::min(0.90, entry_price + 0.08), 0.50, false});
        levels.push_back({1, std::min(0.92, entry_price + 0.15), 1.00, false});
    } else if (regime == StrategyRegime::QUIET_REVERSION) {
        levels.push_back({0, 0.42, 0.75, false});
        levels.push_back({1, 0.62, 1.00, false});
    } else {
        levels.push_back({0, 0.45, 0.75, false});
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

std::vector<TakeProfitLevel> ExperimentStrategy::compute_eth_only_tp_levels(
    double entry_price, StrategyRegime regime) const {
    if (regime == StrategyRegime::TREND) {
        return compute_trend_follow_tp_levels(entry_price);
    }

    std::vector<TakeProfitLevel> levels;
    levels.push_back({0, std::min(0.95, entry_price + 0.10), 0.40, false});
    levels.push_back({1, std::min(0.95, entry_price + 0.20), 0.50, false});
    return levels;
}

std::vector<TakeProfitLevel> ExperimentStrategy::compute_late_window_tp_levels(
    double entry_price) const {
    std::vector<TakeProfitLevel> levels;
    levels.push_back({0, std::min(0.90, entry_price + 0.06), 0.50, false});
    levels.push_back({1, std::min(0.92, entry_price + 0.12), 0.50, false});
    return levels;
}

std::vector<TakeProfitLevel> ExperimentStrategy::compute_eth_late_cheap_tp_levels(
    double entry_price) const {
    std::vector<TakeProfitLevel> levels;
    levels.push_back({0, std::min(0.95, entry_price + 0.08), 0.50, false});
    levels.push_back({1, std::min(0.95, entry_price + 0.16), 0.50, false});
    return levels;
}

std::vector<TakeProfitLevel> ExperimentStrategy::compute_finance_updown_tp_levels(
    double entry_price, StrategyRegime regime) const {
    std::vector<TakeProfitLevel> levels;
    if (regime == StrategyRegime::REVERSAL) {
        levels.push_back({0, std::min(0.95, entry_price + 0.10), 0.60, false});
        levels.push_back({1, std::min(0.95, entry_price + 0.20), 1.00, false});
    } else {
        levels.push_back({0, std::min(0.90, entry_price + 0.08), 0.50, false});
        levels.push_back({1, std::min(0.92, entry_price + 0.15), 0.50, false});
    }
    return levels;
}

std::vector<TakeProfitLevel> ExperimentStrategy::compute_crypto_duration_tp_levels(
    double entry_price, bool daily) const {
    std::vector<TakeProfitLevel> levels;
    if (daily) {
        levels.push_back({0, std::min(0.92, entry_price + 0.08), 0.35, false});
        levels.push_back({1, std::min(0.94, entry_price + 0.16), 0.54, false});
    } else {
        levels.push_back({0, std::min(0.90, entry_price + 0.08), 0.40, false});
        levels.push_back({1, std::min(0.92, entry_price + 0.15), 0.58, false});
    }
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

    if (pos.regime == StrategyRegime::LEGACY_CHEAP) {
        double max_adverse = pos.entry_price - pos.min_price;
        double loss_pct = (pos.entry_price - current_contract_price) / pos.entry_price;
        if (loss_pct >= 0.30) {
            exit.should_exit = true; exit.reason = "stop_price"; exit.exit_price = current_contract_price; return exit;
        }
        if (elapsed_sec >= 90 && mfe < 0.015 &&
            current_contract_price <= pos.entry_price - 0.02) {
            exit.should_exit = true; exit.reason = "cheap_fail_stop"; exit.exit_price = current_contract_price; return exit;
        }
        if (elapsed_sec >= 150 && mfe < 0.03) {
            exit.should_exit = true; exit.reason = "cheap_fail_stop"; exit.exit_price = current_contract_price; return exit;
        }
        if (max_adverse >= 0.06 && mfe < 0.10) {
            exit.should_exit = true; exit.reason = "adverse_expansion_stop"; exit.exit_price = current_contract_price; return exit;
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
    const BtcMarketData& md,
    const TrendFollowContext& ctx) const {
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

    if (elapsed_sec >= 150 && mfe < 0.03 &&
        current_contract_price <= pos.entry_price - 0.03) {
        exit.should_exit = true;
        exit.reason = "no_start_exit";
        exit.exit_price = current_contract_price;
        return exit;
    }

    const BtcMarketData* btc_md = ctx.has_btc ? &ctx.btc : nullptr;
    const BtcMarketData* eth_md = ctx.has_eth ? &ctx.eth : nullptr;
    std::string btc_align = trend_alignment(pos.side, btc_md);
    std::string eth_align = trend_alignment(pos.side, eth_md);
    if ((btc_align == "opposed" || eth_align == "opposed") &&
        current_contract_price <= pos.entry_price) {
        exit.should_exit = true;
        exit.reason = "no_start_exit";
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

ExitSignal ExperimentStrategy::evaluate_eth_only_exit(
    const Position& pos,
    double current_contract_price,
    const BtcMarketData& md) const {
    ExitSignal exit;
    int64_t elapsed_sec = (wall_now_ms() - pos.entry_time) / 1000;
    double mfe = pos.max_price - pos.entry_price;

    if (elapsed_sec >= 150 && mfe < 0.03 &&
        current_contract_price <= pos.entry_price - 0.03) {
        exit.should_exit = true;
        exit.reason = "no_start_exit";
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

    if (pos.regime == StrategyRegime::TREND) {
        if (current_contract_price <= pos.entry_price - 0.07) {
            exit.should_exit = true;
            exit.reason = "stop_price";
            exit.exit_price = current_contract_price;
            return exit;
        }
        double entry_dev = pos.btc_strike_at_entry > 0
            ? (pos.btc_price_at_entry - pos.btc_strike_at_entry) /
                pos.btc_strike_at_entry * 100.0
            : 0.0;
        bool dev_faded =
            (pos.side == Side::UP && md.deviation_pct < entry_dev - 0.08) ||
            (pos.side == Side::DOWN && md.deviation_pct > entry_dev + 0.08);
        if (dev_faded && current_contract_price <= pos.entry_price) {
            exit.should_exit = true;
            exit.reason = "stop_btc";
            exit.exit_price = current_contract_price;
            return exit;
        }
        if (mfe >= 0.15) {
            double stop = std::max(pos.entry_price + 0.05, pos.max_price - 0.05);
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
        return exit;
    }

    double loss_pct = (pos.entry_price - current_contract_price) / pos.entry_price;
    if (loss_pct >= 0.30) {
        exit.should_exit = true;
        exit.reason = "stop_price";
        exit.exit_price = current_contract_price;
        return exit;
    }
    if (mfe >= 0.20) {
        double stop = std::max(pos.entry_price + 0.08, pos.max_price - 0.08);
        if (current_contract_price <= stop) {
            exit.should_exit = true;
            exit.reason = "trailing_stop";
            exit.exit_price = current_contract_price;
            return exit;
        }
    }
    if (md.minutes_remaining <= 10 && current_contract_price < 0.25) {
        exit.should_exit = true;
        exit.reason = "stop_time";
        exit.exit_price = current_contract_price;
        return exit;
    }
    return exit;
}

ExitSignal ExperimentStrategy::evaluate_late_window_exit(
    const Position& pos,
    double current_contract_price,
    const BtcMarketData& md) const {
    ExitSignal exit;
    int64_t elapsed_sec = (wall_now_ms() - pos.entry_time) / 1000;
    double mfe = pos.max_price - pos.entry_price;

    if (elapsed_sec >= 60 && mfe < 0.03 &&
        current_contract_price <= pos.entry_price - 0.02) {
        exit.should_exit = true;
        exit.reason = "late_no_start_exit";
        exit.exit_price = current_contract_price;
        return exit;
    }

    if (current_contract_price <= pos.entry_price - 0.05) {
        exit.should_exit = true;
        exit.reason = "late_stop_price";
        exit.exit_price = current_contract_price;
        return exit;
    }

    double entry_dev = pos.btc_strike_at_entry > 0
        ? (pos.btc_price_at_entry - pos.btc_strike_at_entry) /
            pos.btc_strike_at_entry * 100.0
        : 0.0;
    bool dev_faded =
        (pos.side == Side::UP && md.deviation_pct < entry_dev - 0.10) ||
        (pos.side == Side::DOWN && md.deviation_pct > entry_dev + 0.10);
    if (dev_faded && current_contract_price <= pos.entry_price) {
        exit.should_exit = true;
        exit.reason = "late_dev_fade";
        exit.exit_price = current_contract_price;
        return exit;
    }

    bool has_tp = std::any_of(pos.tp_levels.begin(), pos.tp_levels.end(),
        [](const TakeProfitLevel& tp) { return tp.triggered; });
    if (has_tp) {
        double stop = std::max(pos.entry_price + 0.02, pos.max_price - 0.05);
        if (current_contract_price <= stop) {
            exit.should_exit = true;
            exit.reason = "late_trailing_stop";
            exit.exit_price = current_contract_price;
            return exit;
        }
    }

    if (md.minutes_remaining <= 5 && current_contract_price < 0.60) {
        exit.should_exit = true;
        exit.reason = "late_time_exit";
        exit.exit_price = current_contract_price;
        return exit;
    }

    return exit;
}

ExitSignal ExperimentStrategy::evaluate_eth_late_cheap_exit(
    const Position& pos,
    double current_contract_price,
    const BtcMarketData& md) const {
    ExitSignal exit;
    int64_t elapsed_sec = (wall_now_ms() - pos.entry_time) / 1000;
    double mfe = pos.max_price - pos.entry_price;

    if (elapsed_sec >= 60 && mfe < 0.03 &&
        current_contract_price <= pos.entry_price - 0.02) {
        exit.should_exit = true;
        exit.reason = "eth_late_no_start_exit";
        exit.exit_price = current_contract_price;
        return exit;
    }

    if (current_contract_price <= pos.entry_price - 0.05) {
        exit.should_exit = true;
        exit.reason = "eth_late_stop_price";
        exit.exit_price = current_contract_price;
        return exit;
    }

    bool has_tp = std::any_of(pos.tp_levels.begin(), pos.tp_levels.end(),
        [](const TakeProfitLevel& tp) { return tp.triggered; });
    if (has_tp) {
        double stop = std::max(pos.entry_price + 0.02, pos.max_price - 0.06);
        if (current_contract_price <= stop) {
            exit.should_exit = true;
            exit.reason = "eth_late_trailing_stop";
            exit.exit_price = current_contract_price;
            return exit;
        }
    }

    if (md.minutes_remaining <= 8 && current_contract_price < pos.entry_price + 0.08) {
        exit.should_exit = true;
        exit.reason = "eth_late_time_exit";
        exit.exit_price = current_contract_price;
        return exit;
    }

    return exit;
}

ExitSignal ExperimentStrategy::evaluate_finance_updown_exit(
    const Position& pos,
    double current_contract_price,
    const BtcMarketData& md) const {
    ExitSignal exit;
    int64_t elapsed_sec = (wall_now_ms() - pos.entry_time) / 1000;
    double mfe = pos.max_price - pos.entry_price;

    if (pos.regime == StrategyRegime::REVERSAL) {
        double loss_pct = (pos.entry_price - current_contract_price) / pos.entry_price;
        if (loss_pct >= 0.35) {
            exit.should_exit = true;
            exit.reason = "finance_reversal_stop";
            exit.exit_price = current_contract_price;
            return exit;
        }
        if (elapsed_sec >= 300 && mfe < 0.03) {
            exit.should_exit = true;
            exit.reason = "finance_reversal_no_start";
            exit.exit_price = current_contract_price;
            return exit;
        }
    }

    if (elapsed_sec >= 180 && mfe < 0.03 &&
        current_contract_price <= pos.entry_price - 0.03) {
        exit.should_exit = true;
        exit.reason = "finance_no_start_exit";
        exit.exit_price = current_contract_price;
        return exit;
    }

    if (pos.regime != StrategyRegime::REVERSAL &&
        current_contract_price <= pos.entry_price - 0.07) {
        exit.should_exit = true;
        exit.reason = "finance_stop_price";
        exit.exit_price = current_contract_price;
        return exit;
    }

    bool has_tp = std::any_of(pos.tp_levels.begin(), pos.tp_levels.end(),
        [](const TakeProfitLevel& tp) { return tp.triggered; });
    if (has_tp) {
        double stop = std::max(pos.entry_price + 0.02, pos.max_price - 0.06);
        if (current_contract_price <= stop) {
            exit.should_exit = true;
            exit.reason = "finance_trailing_stop";
            exit.exit_price = current_contract_price;
            return exit;
        }
    }

    if (md.minutes_remaining <= 8 && current_contract_price < pos.entry_price + 0.04) {
        exit.should_exit = true;
        exit.reason = "finance_time_exit";
        exit.exit_price = current_contract_price;
        return exit;
    }

    return exit;
}

ExitSignal ExperimentStrategy::evaluate_crypto_duration_exit(
    const Position& pos,
    double current_contract_price,
    const BtcMarketData& md,
    bool daily) const {
    ExitSignal exit;
    int64_t elapsed_sec = (wall_now_ms() - pos.entry_time) / 1000;
    double mfe = pos.max_price - pos.entry_price;
    int no_start_sec = daily ? 1800 : 600;
    double no_start_mfe = daily ? 0.04 : 0.03;
    double no_start_loss = daily ? 0.04 : 0.03;
    if (elapsed_sec >= no_start_sec && mfe < no_start_mfe &&
        current_contract_price <= pos.entry_price - no_start_loss) {
        exit.should_exit = true;
        exit.reason = daily ? "crypto_daily_no_start" : "crypto_4h_no_start";
        exit.exit_price = current_contract_price;
        return exit;
    }

    double stop_loss = daily ? 0.10 : 0.08;
    if (current_contract_price <= pos.entry_price - stop_loss) {
        exit.should_exit = true;
        exit.reason = daily ? "crypto_daily_stop" : "crypto_4h_stop";
        exit.exit_price = current_contract_price;
        return exit;
    }

    bool dev_crossed_zero =
        (pos.side == Side::UP && md.deviation_pct <= 0.05) ||
        (pos.side == Side::DOWN && md.deviation_pct >= -0.05);
    if (dev_crossed_zero && current_contract_price <= pos.entry_price) {
        exit.should_exit = true;
        exit.reason = daily ? "crypto_daily_trend_invalid" : "crypto_4h_trend_invalid";
        exit.exit_price = current_contract_price;
        return exit;
    }

    bool has_tp = std::any_of(pos.tp_levels.begin(), pos.tp_levels.end(),
        [](const TakeProfitLevel& tp) { return tp.triggered; });
    if (has_tp) {
        double stop = std::max(pos.entry_price + 0.02, pos.max_price - (daily ? 0.08 : 0.06));
        if (current_contract_price <= stop) {
            exit.should_exit = true;
            exit.reason = daily ? "crypto_daily_trailing_stop" : "crypto_4h_trailing_stop";
            exit.exit_price = current_contract_price;
            return exit;
        }
    }

    if (daily && md.minutes_remaining <= 60 && current_contract_price < pos.entry_price + 0.05) {
        exit.should_exit = true;
        exit.reason = "crypto_daily_time_exit";
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
    candidate_log_path_ = log_path_;
    auto suffix = candidate_log_path_.rfind("_trades.jsonl");
    if (suffix != std::string::npos) {
        candidate_log_path_.replace(suffix, std::string("_trades.jsonl").size(),
                                    "_candidates.jsonl");
    } else {
        candidate_log_path_ += ".candidates.jsonl";
    }
    for (const auto& coin : cfg_.coins) {
        strategies_.emplace(coin.coin, ExperimentStrategy(cfg_, coin.coin));
    }
    if (strategy_name_ == "finance_updown_v1") {
        for (const auto& asset : {"SPY", "SPX", "GOLD", "WTI"}) {
            strategies_.emplace(asset, ExperimentStrategy(cfg_, asset));
        }
    }
    if (strategy_name_ == "crypto_4h_updown_v1" ||
        strategy_name_ == "crypto_daily_updown_v1") {
        for (const auto& coin : {"BTC", "ETH", "SOL", "XRP", "BNB"}) {
            strategies_.emplace(coin, ExperimentStrategy(cfg_, coin));
        }
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
    latest_market_data_[coin] = md;
    TrendFollowContext trend_ctx;
    auto btc_latest = latest_market_data_.find("BTC");
    if (btc_latest != latest_market_data_.end()) {
        trend_ctx.has_btc = true;
        trend_ctx.btc = btc_latest->second;
    }
    auto eth_latest = latest_market_data_.find("ETH");
    if (eth_latest != latest_market_data_.end()) {
        trend_ctx.has_eth = true;
        trend_ctx.eth = eth_latest->second;
    }
    auto strat_it = strategies_.find(coin);
    if (strat_it == strategies_.end()) return;

    auto close_expired = [&](Position& pos) {
        bool below_shape = strategy_name_ == "finance_updown_v1" &&
            pos.confidence_components.find("below_shape") != std::string::npos;
        bool won = below_shape
            ? ((pos.side == Side::UP && md.current_price <= pos.btc_strike_at_entry) ||
               (pos.side == Side::DOWN && md.current_price > pos.btc_strike_at_entry))
            : ((pos.side == Side::UP && md.current_price > pos.btc_strike_at_entry) ||
               (pos.side == Side::DOWN && md.current_price <= pos.btc_strike_at_entry));
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
        if (strategy_name_ == "finance_updown_v1" && stale_market) continue;
        if (stale_market || md.minutes_remaining <= 0) {
            close_expired(pos);
            continue;
        }
        double current_price = pos.side == Side::UP ? quotes.up_bid : quotes.down_bid;
        if (current_price <= 0) continue;
        pos.current_price = current_price;
        pos.max_price = std::max(pos.max_price, current_price);
        pos.min_price = std::min(pos.min_price, current_price);
        // Trail 评估埋点：影子策略没有 trail，价格路径完整跑完，正好用来反演 trail 会不会
        // 误杀赢家。首次到 entry+0.05 记武装时刻，之后跟踪武装后最低 bid。
        if (pos.armed_at_ms == 0 && current_price >= pos.entry_price + 0.05) {
            pos.armed_at_ms = now_ms;
            pos.min_price_after_arm = current_price;
        } else if (pos.armed_at_ms != 0 && current_price < pos.min_price_after_arm) {
            pos.min_price_after_arm = current_price;
        }
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

        double taker_rate = strategy_name_ == "finance_updown_v1"
            ? cfg_.fees.finance_taker_fee_rate : cfg_.fees.taker_fee_rate;
        for (auto& tp : pos.tp_levels) {
            if (tp.triggered || current_price < tp.trigger_price) continue;
            if (pos.shares_remaining_pct < 0.01) break;  // 已清仓（含 dust 规则）无份额可卖
            double sell_shares = pos.shares * pos.shares_remaining_pct * tp.sell_pct;
            // 交易所最小单：市价卖单 <$1 现实下不出。若这一档或卖后剩余 <$1，则一次性清掉剩余整仓。
            double remaining_now = pos.shares * pos.shares_remaining_pct;
            if (sell_shares * current_price < cfg_.fees.min_order_usdc ||
                (remaining_now - sell_shares) * current_price < cfg_.fees.min_order_usdc) {
                sell_shares = remaining_now;
            }
            double sell_value = sell_shares * current_price;
            double cost_basis = sell_shares * pos.entry_price;
            double exit_fee = fee(sell_shares, current_price, taker_rate, cfg_.fees);
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

        ExitSignal exit;
        if (strategy_name_ == "finance_updown_v1") {
            exit = strat_it->second.evaluate_finance_updown_exit(pos, current_price, md);
        } else if (strategy_name_ == "crypto_4h_updown_v1" ||
                   strategy_name_ == "crypto_daily_updown_v1") {
            exit = strat_it->second.evaluate_crypto_duration_exit(
                pos, current_price, md, strategy_name_ == "crypto_daily_updown_v1");
        } else if (strategy_name_ == "eth_late_cheap_v1") {
            exit = strat_it->second.evaluate_eth_late_cheap_exit(pos, current_price, md);
        } else if (strategy_name_ == "late_window_v1") {
            exit = strat_it->second.evaluate_late_window_exit(pos, current_price, md);
        } else if (strategy_name_ == "eth_only_v1" || strategy_name_ == "eth_cheap_v1") {
            exit = strat_it->second.evaluate_eth_only_exit(pos, current_price, md);
        } else if (strategy_name_ == "trend_follow" || strategy_name_ == "trend_v3") {
            exit = strat_it->second.evaluate_trend_follow_exit(pos, current_price, md, trend_ctx);
        } else {
            exit = strat_it->second.evaluate_exit(pos, current_price, md);
        }
        if (exit.should_exit) {
            double remaining_shares = pos.shares * pos.shares_remaining_pct;
            double sell_value = remaining_shares * exit.exit_price;
            double cost_basis = remaining_shares * pos.entry_price;
            double exit_fee = fee(remaining_shares, exit.exit_price, taker_rate, cfg_.fees);
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
    if (strategy_name_ == "finance_updown_v1") {
        sig = strat_it->second.evaluate_finance_updown_v1(md, quotes, entry.market.condition_id,
                                                         entry.market.question);
    } else if (strategy_name_ == "crypto_4h_updown_v1" ||
               strategy_name_ == "crypto_daily_updown_v1") {
        sig = strat_it->second.evaluate_crypto_duration_updown_v1(
            md, quotes, entry.market.condition_id, entry.market.question,
            strategy_name_ == "crypto_daily_updown_v1");
    } else if (strategy_name_ == "eth_late_cheap_v1") {
        sig = strat_it->second.evaluate_eth_late_cheap_v1(md, quotes, entry.market.condition_id,
                                                          entry.market.question);
    } else if (strategy_name_ == "late_window_v1") {
        sig = strat_it->second.evaluate_late_window_v1(md, quotes, entry.market.condition_id,
                                                       entry.market.question);
    } else if (strategy_name_ == "eth_cheap_v1") {
        sig = strat_it->second.evaluate_eth_cheap_v1(md, quotes, entry.market.condition_id,
                                                     entry.market.question);
    } else if (strategy_name_ == "eth_only_v1") {
        sig = strat_it->second.evaluate_eth_only_v1(md, quotes, entry.market.condition_id,
                                                    entry.market.question);
    } else if (strategy_name_ == "trend_follow") {
        sig = strat_it->second.evaluate_trend_follow(md, quotes, entry.market.condition_id,
                                                     entry.market.question, trend_ctx);
    } else if (strategy_name_ == "trend_v3") {
        sig = strat_it->second.evaluate_trend_v3(md, quotes, entry.market.condition_id,
                                                 entry.market.question, trend_ctx, now_ms);
    } else if (strategy_name_ == "legacy_cheap_v2") {
        sig = strat_it->second.evaluate_legacy_cheap_v2(md, quotes, entry.market.condition_id,
                                                        entry.market.question, trend_ctx);
    } else {
        sig = strat_it->second.evaluate_entry(md, quotes, entry.market.condition_id,
                                              entry.market.question);
    }
    if (!sig.valid) {
        if (!sig.reject_reason.empty()) reject_counts_[sig.reject_reason]++;
        record_candidate(coin, md, entry, quotes, sig, now_ms);
        return;
    }
    if (sig.regime == StrategyRegime::QUIET_REVERSION &&
        quiet_trades_this_hour_ >= cfg_.strategy.max_quiet_trades_per_hour) {
        reject_counts_["quiet_hour_trades"]++;
        return;
    }
    // 交易所最小单（执行层约束，不动策略决策）：限价买单 ≥ min_order_shares 股，
    // 且名义额 ≥ min_order_usdc（保证离场可市价卖出）。份额托到下限后重算 size。
    {
        double min_shares = std::max(cfg_.fees.min_order_shares,
                                     cfg_.fees.min_order_usdc / sig.entry_price);
        if (sig.shares < min_shares) {
            sig.shares = min_shares;
            sig.size_usdc = sig.shares * sig.entry_price;
        }
    }
    if (sig.size_usdc > balance_) {
        reject_counts_["insufficient_balance"]++;
        auto rejected = sig;
        rejected.valid = false;
        rejected.reject_reason = "insufficient_balance";
        record_candidate(coin, md, entry, quotes, rejected, now_ms);
        return;
    }
    record_candidate(coin, md, entry, quotes, sig, now_ms);

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
    if (strategy_name_ == "finance_updown_v1") {
        pos.tp_levels = strat_it->second.compute_finance_updown_tp_levels(sig.entry_price, sig.regime);
    } else if (strategy_name_ == "crypto_4h_updown_v1" ||
               strategy_name_ == "crypto_daily_updown_v1") {
        pos.tp_levels = strat_it->second.compute_crypto_duration_tp_levels(
            sig.entry_price, strategy_name_ == "crypto_daily_updown_v1");
    } else if (strategy_name_ == "eth_late_cheap_v1") {
        pos.tp_levels = strat_it->second.compute_eth_late_cheap_tp_levels(sig.entry_price);
    } else if (strategy_name_ == "late_window_v1") {
        pos.tp_levels = strat_it->second.compute_late_window_tp_levels(sig.entry_price);
    } else if (strategy_name_ == "eth_only_v1" || strategy_name_ == "eth_cheap_v1") {
        pos.tp_levels = strat_it->second.compute_eth_only_tp_levels(sig.entry_price, sig.regime);
    } else if (strategy_name_ == "trend_follow" || strategy_name_ == "trend_v3") {
        pos.tp_levels = strat_it->second.compute_trend_follow_tp_levels(sig.entry_price);
    } else {
        pos.tp_levels = strat_it->second.compute_tp_levels(sig.entry_price, sig.regime);
    }
    pos.max_price = sig.entry_price;
    pos.min_price = sig.entry_price;
    pos.mfe_at_5min = sig.entry_price;
    pos.mfe_at_10min = sig.entry_price;
    pos.mfe_at_15min = sig.entry_price;
    pos.spread_at_entry = sig.side == Side::UP
        ? quotes.up_ask - quotes.up_bid : quotes.down_ask - quotes.down_bid;
    pos.entry_confidence = sig.entry_confidence;
    pos.btc_alignment = sig.btc_alignment;
    pos.eth_alignment = sig.eth_alignment;
    pos.cross_coin_state = sig.cross_coin_state;
    pos.entry_price_bucket = sig.entry_price_bucket;
    pos.confidence_components = sig.confidence_components;
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

void ExperimentEngine::record_candidate(const std::string& coin,
                                        const BtcMarketData& md,
                                        const MarketEntry& entry,
                                        const ExperimentQuotes& quotes,
                                        const EntrySignal& sig,
                                        int64_t now_ms) const {
    if (sig.reject_reason.find("coin_filter") != std::string::npos) return;
    nlohmann::json j;
    j["time"] = now_ms;
    j["strategy"] = strategy_name_;
    j["coin"] = coin;
    j["market"] = entry.market.question;
    j["condition_id"] = entry.market.condition_id;
    j["accepted"] = sig.valid;
    j["reject_reason"] = sig.reject_reason;
    j["side"] = side_name(sig.side);
    j["regime"] = regime_name_local(sig.regime);
    j["entry_price"] = sig.entry_price;
    j["size_usdc"] = sig.size_usdc;
    j["current_price"] = md.current_price;
    j["reference_price"] = md.strike_price;
    j["deviation_pct"] = md.deviation_pct;
    j["minutes_remaining"] = md.minutes_remaining;
    j["up_bid"] = quotes.up_bid;
    j["up_ask"] = quotes.up_ask;
    j["down_bid"] = quotes.down_bid;
    j["down_ask"] = quotes.down_ask;
    j["up_spread"] = quotes.up_ask > 0 && quotes.up_bid > 0 ? quotes.up_ask - quotes.up_bid : 0;
    j["down_spread"] = quotes.down_ask > 0 && quotes.down_bid > 0 ? quotes.down_ask - quotes.down_bid : 0;
    j["entry_price_bucket"] = sig.entry_price_bucket;
    j["confidence_components"] = sig.confidence_components;
    std::ofstream out(candidate_log_path_, std::ios::app);
    if (out) out << j.dump() << "\n";
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
    rec.armed_at_ms = pos.armed_at_ms;
    rec.min_price_after_arm = pos.min_price_after_arm;
    if (pos.entry_price > 0) {
        rec.mfe5_gain_pct = (pos.mfe_at_5min - pos.entry_price) / pos.entry_price;
        rec.mfe10_gain_pct = (pos.mfe_at_10min - pos.entry_price) / pos.entry_price;
        rec.mfe15_gain_pct = (pos.mfe_at_15min - pos.entry_price) / pos.entry_price;
    }
    rec.strategy = strategy_name_;
    rec.entry_confidence = pos.entry_confidence;
    rec.btc_alignment = pos.btc_alignment;
    rec.eth_alignment = pos.eth_alignment;
    rec.cross_coin_state = pos.cross_coin_state;
    rec.entry_price_bucket = pos.entry_price_bucket;
    rec.confidence_components = pos.confidence_components;
    rec.max_favorable = pos.max_price - pos.entry_price;
    rec.max_adverse = pos.entry_price - pos.min_price;
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
        x["entry_confidence"] = p.entry_confidence;
        x["btc_alignment"] = p.btc_alignment;
        x["eth_alignment"] = p.eth_alignment;
        x["cross_coin_state"] = p.cross_coin_state;
        x["entry_price_bucket"] = p.entry_price_bucket;
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
        j["entry_confidence"] = t.entry_confidence;
        j["btc_alignment"] = t.btc_alignment;
        j["eth_alignment"] = t.eth_alignment;
        j["cross_coin_state"] = t.cross_coin_state;
        j["entry_price_bucket"] = t.entry_price_bucket;
        j["max_favorable"] = t.max_favorable;
        j["max_adverse"] = t.max_adverse;
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
