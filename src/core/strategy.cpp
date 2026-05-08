#include "core/strategy.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

#include <spdlog/spdlog.h>

namespace polymarket {

Strategy::Strategy(const AppConfig& cfg) : cfg_(cfg) {}

Strategy::Strategy(const AppConfig& cfg, std::string coin)
    : cfg_(cfg), coin_(std::move(coin)) {}

double Strategy::max_entry_price(int minutes_remaining) const {
    // §一：剩余35分钟以上，ask <= 30¢
    if (minutes_remaining > 35) return 0.30;
    return 0;  // 不入场
}

namespace {

struct LegacyEntryFilter {
    double min_entry = 0.18;
    double max_entry = 0.26;
    double max_abs_dev = 0.30;
    double max_spread = 0.03;
};

bool is_core_coin(const std::string& coin) {
    return coin == "BTC" || coin == "ETH" || coin == "SOL";
}

bool side_aligned(Side side, double deviation_pct) {
    return (side == Side::UP && deviation_pct > 0) ||
           (side == Side::DOWN && deviation_pct < 0);
}

LegacyEntryFilter legacy_filter_for_coin(const std::string& coin) {
    if (coin == "BTC" || coin == "SOL") {
        return {0.18, 0.26, 0.30, 0.03};
    }
    if (coin == "ETH") {
        return {0.20, 0.26, 0.25, 0.02};
    }
    return {0.20, 0.24, 0.20, 0.02};
}

}  // namespace

// 计算费后回本价：在 sell_price 卖出时，扣除买卖双向手续费后刚好不亏
// 手续费公式：shares × 0.05 × p × (1-p)
// 买入成本/share = entry_price + 0.05 × entry_price × (1 - entry_price)
// 卖出收入/share = sell_price - 0.05 × sell_price × (1 - sell_price)
// 回本条件：卖出收入 = 买入成本
// 即：P - 0.05P(1-P) = E + 0.05E(1-E)
// 0.05P² + 0.95P = E(1.05 - 0.05E)
// 解二次方程
static double breakeven_price(double entry_price) {
    double rhs = entry_price * (1.05 - 0.05 * entry_price);
    // 0.05P² + 0.95P - rhs = 0
    double a = 0.05, b = 0.95, c = -rhs;
    double disc = b * b - 4 * a * c;
    if (disc < 0) return entry_price * 1.05;  // fallback
    return (-b + std::sqrt(disc)) / (2 * a);
}

EntrySignal Strategy::evaluate_cheap_rebound(
    const BtcMarketData& btc,
    double up_bid, double up_ask,
    double down_bid, double down_ask,
    const std::string& up_token_id, const std::string& down_token_id,
    const std::string& condition_id, const std::string& question,
    int minutes_remaining) {

    EntrySignal sig;
    sig.condition_id = condition_id;
    sig.market_question = question;
    sig.coin = coin_;
    sig.regime = StrategyRegime::QUIET_REVERSION;

    if (minutes_remaining < 36 || minutes_remaining > 45) {
        sig.reject_reason = "cheap_rebound_time_window";
        return sig;
    }
    if (std::abs(btc.deviation_pct) > 0.12) {
        sig.reject_reason = "cheap_rebound_dev_not_quiet";
        return sig;
    }

    Side candidate_side = Side::NONE;
    double candidate_ask = 0;
    double candidate_bid = 0;
    std::string candidate_token;
    if (up_ask > 0 && (down_ask <= 0 || up_ask <= down_ask)) {
        candidate_side = Side::UP;
        candidate_ask = up_ask;
        candidate_bid = up_bid;
        candidate_token = up_token_id;
    } else if (down_ask > 0) {
        candidate_side = Side::DOWN;
        candidate_ask = down_ask;
        candidate_bid = down_bid;
        candidate_token = down_token_id;
    } else {
        sig.reject_reason = "no_valid_ask";
        return sig;
    }

    if (candidate_bid <= 0 || candidate_ask <= 0 || candidate_ask > 0.29) {
        sig.reject_reason = "cheap_rebound_invalid_price";
        return sig;
    }
    double spread = candidate_ask - candidate_bid;
    if (spread < 0 || spread > 0.010001) {
        sig.reject_reason = "cheap_rebound_spread_wide";
        return sig;
    }

    double entry_price = std::max(0.01, candidate_ask - 0.01);
    double min_entry = is_core_coin(coin_) ? 0.22 : 0.24;
    double max_entry = 0.28;
    double max_abs_dev = is_core_coin(coin_) ? 0.12 : 0.08;
    if (entry_price < min_entry || entry_price > max_entry) {
        sig.reject_reason = "cheap_rebound_entry_range";
        return sig;
    }
    if (std::abs(btc.deviation_pct) > max_abs_dev) {
        sig.reject_reason = "cheap_rebound_coin_dev_too_large";
        return sig;
    }

    sig.valid = true;
    sig.side = candidate_side;
    sig.market_ask = candidate_ask;
    sig.entry_price = entry_price;
    sig.size_usdc = 1.00;
    sig.shares = sig.size_usdc / sig.entry_price;
    sig.token_id = candidate_token;
    spdlog::info("SIGNAL [{} cheap_rebound]: {} {} @ {:.3f} (ask={:.3f}, spread={:.3f}, dev={:+.2f}%)",
                 coin_, sig.side == Side::UP ? "UP" : "DOWN", question,
                 sig.entry_price, candidate_ask, spread, btc.deviation_pct);
    return sig;
}

EntrySignal Strategy::evaluate_momentum_follow(
    const BtcMarketData& btc,
    double up_bid, double up_ask,
    double down_bid, double down_ask,
    const std::string& up_token_id, const std::string& down_token_id,
    const std::string& condition_id, const std::string& question,
    int minutes_remaining) {

    EntrySignal sig;
    sig.condition_id = condition_id;
    sig.market_question = question;
    sig.coin = coin_;
    sig.regime = StrategyRegime::TREND;

    if (minutes_remaining < 32 || minutes_remaining > 42) {
        sig.reject_reason = "momentum_time_window";
        return sig;
    }
    double abs_dev = std::abs(btc.deviation_pct);
    if (abs_dev < 0.20) {
        sig.reject_reason = "momentum_dev_too_small";
        return sig;
    }

    Side side = btc.deviation_pct > 0 ? Side::UP : Side::DOWN;
    double ask = side == Side::UP ? up_ask : down_ask;
    double bid = side == Side::UP ? up_bid : down_bid;
    std::string token = side == Side::UP ? up_token_id : down_token_id;
    if (bid <= 0 || ask <= 0) {
        sig.reject_reason = "momentum_invalid_quote";
        return sig;
    }
    double spread = ask - bid;
    if (spread < 0 || spread > 0.010001) {
        sig.reject_reason = "momentum_spread_wide";
        return sig;
    }

    double entry_price = std::max(0.01, ask - 0.01);
    if (entry_price < 0.64 || entry_price > 0.69) {
        sig.reject_reason = "momentum_entry_range";
        return sig;
    }
    if (entry_price > 0.68 && abs_dev < 0.24) {
        sig.reject_reason = "momentum_high_entry_dev_weak";
        return sig;
    }
    if (!is_core_coin(coin_) && abs_dev < 0.24) {
        sig.reject_reason = "momentum_alt_dev_weak";
        return sig;
    }

    sig.valid = true;
    sig.side = side;
    sig.market_ask = ask;
    sig.entry_price = entry_price;
    sig.size_usdc = 1.00;
    sig.shares = sig.size_usdc / sig.entry_price;
    sig.token_id = token;
    spdlog::info("SIGNAL [{} momentum]: {} {} @ {:.3f} (ask={:.3f}, spread={:.3f}, dev={:+.2f}%)",
                 coin_, sig.side == Side::UP ? "UP" : "DOWN", question,
                 sig.entry_price, ask, spread, btc.deviation_pct);
    return sig;
}

EntrySignal Strategy::evaluate_entry(
    const BtcMarketData& btc,
    double up_bid, double up_ask,
    double down_bid, double down_ask,
    const std::string& up_token_id, const std::string& down_token_id,
    const std::string& condition_id, const std::string& question,
    int minutes_remaining) {

    double abs_dev = std::abs(btc.deviation_pct);
    if (abs_dev <= 0.12) {
        return evaluate_cheap_rebound(btc, up_bid, up_ask, down_bid, down_ask,
                                      up_token_id, down_token_id, condition_id,
                                      question, minutes_remaining);
    }
    if (abs_dev >= 0.20) {
        return evaluate_momentum_follow(btc, up_bid, up_ask, down_bid, down_ask,
                                        up_token_id, down_token_id, condition_id,
                                        question, minutes_remaining);
    }

    EntrySignal sig;
    sig.condition_id = condition_id;
    sig.market_question = question;
    sig.coin = coin_;
    sig.reject_reason = "regime_uncertain: abs_dev=" + std::to_string(abs_dev);
    return sig;
}

std::vector<TakeProfitLevel> Strategy::compute_tp_levels(double entry_price) {
    return compute_tp_levels(entry_price, StrategyRegime::LEGACY_CHEAP);
}

std::vector<TakeProfitLevel> Strategy::compute_tp_levels(double entry_price, StrategyRegime) {
    // §四 止盈规则：先锁利润，再保留趋势尾仓
    std::vector<TakeProfitLevel> levels;

    if (entry_price >= 0.60) {
        levels.push_back({0, std::min(0.88, entry_price + 0.08), 0.50, false});
        levels.push_back({1, std::min(0.90, entry_price + 0.14), 0.50, false});
        levels.push_back({2, 0.90, 1.00, false});
        return levels;
    }

    levels.push_back({0, 0.45, 0.50, false});  // TP0: sell 50% of remaining
    levels.push_back({1, 0.70, 0.50, false});  // TP1: sell 50% of remaining
    levels.push_back({2, 0.88, 1.00, false});  // TP2: sell all remaining

    spdlog::debug("TP levels for entry={:.3f}: TP0=0.450(50%) TP1=0.700(50% rem) TP2=0.880(100% rem)",
                  entry_price);

    return levels;
}

ExitSignal Strategy::evaluate_exit(
    const Position& pos,
    double current_contract_price,
    const BtcMarketData& btc,
    int minutes_remaining) {

    ExitSignal exit;
    auto now_chrono = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    int64_t elapsed_sec = (now_chrono - pos.entry_time) / 1000;
    double mfe_gain = pos.max_price - pos.entry_price;

    if (pos.regime == StrategyRegime::TREND) {
        if (current_contract_price <= pos.entry_price - 0.07) {
            exit.should_exit = true;
            exit.reason = "stop_price";
            exit.exit_price = current_contract_price;
            exit.use_market_order = true;
            spdlog::warn("MOMENTUM STOP: {} entry={:.3f} now={:.3f}",
                         pos.market_question, pos.entry_price, current_contract_price);
            return exit;
        }

        double entry_dev = pos.btc_strike_at_entry > 0
            ? (pos.btc_price_at_entry - pos.btc_strike_at_entry) /
                pos.btc_strike_at_entry * 100.0
            : 0.0;
        bool momentum_faded =
            (pos.side == Side::UP && btc.deviation_pct < entry_dev - 0.08) ||
            (pos.side == Side::DOWN && btc.deviation_pct > entry_dev + 0.08);
        if (momentum_faded) {
            exit.should_exit = true;
            exit.reason = "stop_btc";
            exit.exit_price = current_contract_price;
            exit.use_market_order = true;
            return exit;
        }

        if (elapsed_sec >= 150 && mfe_gain < 0.02 &&
            current_contract_price <= pos.entry_price - 0.03) {
            exit.should_exit = true;
            exit.reason = "fast_fail_exit";
            exit.exit_price = current_contract_price;
            exit.use_market_order = true;
            return exit;
        }

        bool has_tp = std::any_of(pos.tp_levels.begin(), pos.tp_levels.end(),
            [](const TakeProfitLevel& tp) { return tp.triggered; });
        if (has_tp && current_contract_price <= pos.entry_price + 0.02) {
            exit.should_exit = true;
            exit.reason = "trailing_stop";
            exit.exit_price = current_contract_price;
            exit.use_market_order = true;
            return exit;
        }

        double mfe = pos.max_price - pos.entry_price;
        if (mfe >= 0.12) {
            double trailing_stop = std::max(pos.entry_price + 0.05, pos.max_price - 0.04);
            if (current_contract_price <= trailing_stop) {
                exit.should_exit = true;
                exit.reason = "trailing_stop";
                exit.exit_price = current_contract_price;
                exit.use_market_order = true;
                return exit;
            }
        } else if (mfe >= 0.08) {
            double trailing_stop = std::max(pos.entry_price + 0.02, pos.max_price - 0.05);
            if (current_contract_price <= trailing_stop) {
                exit.should_exit = true;
                exit.reason = "trailing_stop";
                exit.exit_price = current_contract_price;
                exit.use_market_order = true;
                return exit;
            }
        }

        bool still_aligned = side_aligned(pos.side, btc.deviation_pct);
        if (minutes_remaining <= 5 &&
            (current_contract_price < 0.88 || !still_aligned)) {
            exit.should_exit = true;
            exit.reason = "stop_time";
            exit.exit_price = current_contract_price;
            exit.use_market_order = true;
            return exit;
        }
        if (minutes_remaining <= 8 &&
            (current_contract_price < 0.78 || !still_aligned)) {
            exit.should_exit = true;
            exit.reason = "stop_time";
            exit.exit_price = current_contract_price;
            exit.use_market_order = true;
            return exit;
        }

        return exit;
    }

    // §五.1 价格止损：从入场价下跌 ≥ 30%
    // Step 2.25：阈值从 -50% 收紧到 -30%。
    //   依据：5/1 LIVE 4 笔 stop_price 实际触发时跌幅 -52~-89%（轮询 10s + Polymarket
    //   token 价断崖式下跌，跌穿 -50% 时已经 -89%）。改 -30% 让止损提早触发，即便
    //   lag 也不会到 -50%+。
    double loss_pct = (pos.entry_price - current_contract_price) / pos.entry_price;
    if (loss_pct >= 0.30) {
        exit.should_exit = true;
        exit.reason = "stop_price";
        exit.exit_price = current_contract_price;
        exit.use_market_order = true;
        spdlog::warn("STOP LOSS (price): {} loss={:.1f}% entry={:.3f} now={:.3f}",
                     pos.market_question, loss_pct * 100,
                     pos.entry_price, current_contract_price);
        return exit;
    }

    // §五.1b 死水早退：入场 8 分钟后若 MFE 未启动，立即平仓
    // Step 2.23：阈值从 5¢ 收紧到 2¢（dry 14 笔回放只让 3 笔深死 case 触发）。
    // Step 2.24：窗口从 5min → 8min（实验性单变量调整，观察 5-10 笔再评估）。
    //   触发：5/1 live 4/4 都是"近平卖飞"，无救命 case。两笔反弹时刻：
    //     · 18:10 case：7m00s 涨到 0.43（mfe +20¢） → 8min 窗口能救
    //     · 13:11 case：18+ min 才反弹 → 8min 仍救不了，接受 -$0.36 损失
    //   救命 case（04-29、04-30 15:25）多扛 3 分钟，最坏多损 ~$0.2/笔。
    // Step 2.25：dead_water 加 price floor，避免亏损 -10~-30% 时被 dead_water 误退出
    //   实测 5 笔 dead_water 中 2 笔触发时 current 已 -37/-38%，这种深亏应让
    //   stop_price 接管而非 dead_water"接受任意亏损"。floor=entry × 0.85（亏 ≤15%）
    bool shallow_loss = (current_contract_price >= pos.entry_price * 0.85);
    if (elapsed_sec >= 180 && mfe_gain < 0.02 &&
        current_contract_price <= pos.entry_price - 0.03) {
        exit.should_exit = true;
        exit.reason = "fast_fail_exit";
        exit.exit_price = current_contract_price;
        exit.use_market_order = true;
        spdlog::warn("FAST FAIL EXIT: {} elapsed={}s mfe_gain={:+.3f} entry={:.3f} now={:.3f}",
                     pos.market_question, elapsed_sec, mfe_gain,
                     pos.entry_price, current_contract_price);
        return exit;
    }

    bool has_tp = std::any_of(pos.tp_levels.begin(), pos.tp_levels.end(),
        [](const TakeProfitLevel& tp) { return tp.triggered; });
    if (has_tp && current_contract_price <= pos.entry_price + 0.02) {
        exit.should_exit = true;
        exit.reason = "trailing_stop";
        exit.exit_price = current_contract_price;
        exit.use_market_order = true;
        spdlog::warn("TP PROTECT STOP: {} entry={:.3f} now={:.3f}",
                     pos.market_question, pos.entry_price, current_contract_price);
        return exit;
    }

    if (elapsed_sec >= 480 && mfe_gain < 0.02 && shallow_loss) {
        exit.should_exit = true;
        exit.reason = "dead_water_exit";
        exit.exit_price = current_contract_price;
        exit.use_market_order = true;
        spdlog::warn("DEAD WATER EXIT: {} elapsed={}s mfe_gain={:+.3f} entry={:.3f} now={:.3f}",
                     pos.market_question, elapsed_sec, mfe_gain,
                     pos.entry_price, current_contract_price);
        return exit;
    }

    // §五.2 移动止盈（Trailing Stop）：基于 MFE 动态提升止损线
    // Step 2.20：阈值从 +25/+15 降到 +15/+10，捕获低入场价（$0.10-$0.20）的相对涨幅
    double mfe = pos.max_price - pos.entry_price;
    if (mfe >= 0.15) {
        double trailing_stop = std::max(pos.max_price - 0.07, pos.entry_price + 0.08);
        if (current_contract_price <= trailing_stop) {
            exit.should_exit = true;
            exit.reason = "trailing_stop";
            exit.exit_price = current_contract_price;
            exit.use_market_order = true;
            spdlog::warn("TRAILING STOP (mfe>=15c): {} entry={:.3f} peak={:.3f} now={:.3f} stop={:.3f}",
                         pos.market_question, pos.entry_price, pos.max_price,
                         current_contract_price, trailing_stop);
            return exit;
        }
    } else if (mfe >= 0.10) {
        double trailing_stop = std::max(pos.max_price - 0.08, pos.entry_price + 0.04);
        if (current_contract_price <= trailing_stop) {
            exit.should_exit = true;
            exit.reason = "trailing_stop";
            exit.exit_price = current_contract_price;
            exit.use_market_order = true;
            spdlog::warn("TRAILING STOP (mfe>=10c): {} entry={:.3f} peak={:.3f} now={:.3f} stop={:.3f}",
                         pos.market_question, pos.entry_price, pos.max_price,
                         current_contract_price, trailing_stop);
            return exit;
        }
    }

    // §六 最后10分钟特殊处理（价格止损、移动止盈未触发才走这里）
    if (minutes_remaining <= 10) {
        return evaluate_last_10min(pos, current_contract_price, minutes_remaining);
    }

    return exit;  // should_exit = false
}

ExitSignal Strategy::evaluate_last_10min(
    const Position& pos,
    double current_contract_price,
    int minutes_remaining) {

    ExitSignal exit;

    // §六 最后10分钟特殊处理
    if (current_contract_price < 0.25) {
        // 0-25¢：立即清仓（时间止损）
        exit.should_exit = true;
        exit.reason = "stop_time";
        exit.exit_price = current_contract_price;
        exit.use_market_order = true;
        spdlog::warn("TIME STOP: price={:.3f} < 25¢ with {}min left",
                     current_contract_price, minutes_remaining);
    } else if (current_contract_price >= 0.80) {
        // 80¢以上：持有到期博 $1 结算
        spdlog::debug("HOLD TO EXPIRY: price={:.3f} >= 80¢, {}min left",
                      current_contract_price, minutes_remaining);
    }
    // 25-80¢：继续按止盈规则走

    return exit;
}

}  // namespace polymarket
