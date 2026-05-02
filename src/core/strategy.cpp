#include "core/strategy.h"

#include <chrono>
#include <cmath>

#include <spdlog/spdlog.h>

namespace polymarket {

Strategy::Strategy(const AppConfig& cfg) : cfg_(cfg) {}

double Strategy::max_entry_price(int minutes_remaining) const {
    // §一：剩余30分钟以上，ask < 30¢
    if (minutes_remaining > 30) return 0.30;
    return 0;  // 不入场
}

static const char* regime_name(StrategyRegime regime) {
    switch (regime) {
        case StrategyRegime::TREND: return "trend";
        case StrategyRegime::REVERSAL: return "reversal";
        default: return "none";
    }
}

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

EntrySignal Strategy::evaluate_entry(
    const BtcMarketData& btc,
    double up_ask, double down_ask,
    const std::string& up_token_id, const std::string& down_token_id,
    const std::string& condition_id, const std::string& question,
    int minutes_remaining) {

    EntrySignal sig;
    sig.condition_id = condition_id;
    sig.market_question = question;

    double abs_dev = std::abs(btc.deviation_pct);
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
    constexpr double kSlopeEps = 0.005;  // percentage points, filters one-tick noise
    bool expanding = has_slope &&
        (abs_dev > abs_dev_prev1_ + kSlopeEps) &&
        (abs_dev_prev1_ > abs_dev_prev2_ + kSlopeEps);
    bool contracting = has_slope &&
        (abs_dev + kSlopeEps < abs_dev_prev1_) &&
        (abs_dev_prev1_ + kSlopeEps < abs_dev_prev2_);

    // Update slope state before returning so rejected ticks still build history.
    abs_dev_prev2_ = abs_dev_prev1_;
    abs_dev_prev1_ = abs_dev;
    has_dev_prev2_ = has_dev_prev1_;
    has_dev_prev1_ = true;

    double vol_ratio = (btc.avg_24h_vol > 1e-9) ? btc.current_1h_vol / btc.avg_24h_vol : 0.0;
    if (!has_slope) {
        sig.reject_reason = "regime_warmup";
        return sig;
    }

    Side candidate_side = Side::NONE;
    double candidate_ask = 0;
    std::string candidate_token;
    StrategyRegime regime = StrategyRegime::NONE;
    double size_usdc = 0;

    // Trend mode: BTC deviation expands with enough volatility; buy the favored side.
    if (abs_dev >= 0.18 && expanding && vol_ratio >= 0.8 &&
        minutes_remaining >= 35 && minutes_remaining <= 45) {
        candidate_side = btc.deviation_pct > 0 ? Side::UP : Side::DOWN;
        candidate_ask = candidate_side == Side::UP ? up_ask : down_ask;
        candidate_token = candidate_side == Side::UP ? up_token_id : down_token_id;
        regime = StrategyRegime::TREND;
        size_usdc = (abs_dev >= 0.25 && vol_ratio >= 1.0) ? 2.5 : 2.25;
        if (candidate_ask < 0.50 || candidate_ask > 0.75) {
            sig.reject_reason = "trend_price_window: ask=" + std::to_string(candidate_ask);
            return sig;
        }
    // Reversal mode: BTC deviation is extreme but contracting; buy the cheap contrarian side.
    } else if (abs_dev >= 0.25 && contracting && vol_ratio <= 0.8 &&
               minutes_remaining >= 30 && minutes_remaining <= 38) {
        candidate_side = btc.deviation_pct > 0 ? Side::DOWN : Side::UP;
        candidate_ask = candidate_side == Side::UP ? up_ask : down_ask;
        candidate_token = candidate_side == Side::UP ? up_token_id : down_token_id;
        regime = StrategyRegime::REVERSAL;
        size_usdc = abs_dev >= 0.35 ? 2.5 : 2.25;
        if (candidate_ask > 0.30) {
            sig.reject_reason = "reversal_ask_too_high: ask=" + std::to_string(candidate_ask);
            return sig;
        }
    } else {
        sig.reject_reason = "no_regime: abs_dev=" + std::to_string(abs_dev) +
            " expanding=" + std::to_string(expanding) +
            " contracting=" + std::to_string(contracting) +
            " vol_ratio=" + std::to_string(vol_ratio) +
            " min_left=" + std::to_string(minutes_remaining);
        return sig;
    }

    // 价格条件
    if (candidate_ask <= 0 || candidate_ask >= 1.0) {
        sig.reject_reason = "invalid_ask: " + std::to_string(candidate_ask);
        return sig;
    }

    if (size_usdc <= 0) {
        sig.reject_reason = "invalid_size";
        return sig;
    }

    // 信号有效
    sig.valid = true;
    sig.side = candidate_side;
    sig.regime = regime;
    sig.market_ask = candidate_ask;
    // §二：在当前价下方1¢挂限价买单
    sig.entry_price = candidate_ask - 0.01;
    if (sig.entry_price < 0.01) sig.entry_price = 0.01;
    sig.size_usdc = size_usdc;
    sig.shares = size_usdc / sig.entry_price;
    sig.token_id = candidate_token;

    spdlog::info("SIGNAL [{}]: {} {} @ {:.3f} (ask={:.3f}, size=${:.2f}, BTC dev={:+.2f}%, vol_ratio={:.2f})",
                 regime_name(regime),
                 (sig.side == Side::UP ? "UP" : "DOWN"),
                 question, sig.entry_price, candidate_ask, sig.size_usdc,
                 btc.deviation_pct, vol_ratio);

    return sig;
}

std::vector<TakeProfitLevel> Strategy::compute_tp_levels(double entry_price) {
    return compute_tp_levels(entry_price, StrategyRegime::REVERSAL);
}

std::vector<TakeProfitLevel> Strategy::compute_tp_levels(double entry_price, StrategyRegime regime) {
    // §四 止盈规则（三档减仓，中间档锁住 MFE 浮盈）
    std::vector<TakeProfitLevel> levels;

    if (regime == StrategyRegime::TREND) {
        // Trend entries are higher-priced; use two larger exits so each live FOK
        // sell remains above Polymarket's practical minimum order size.
        levels.push_back({0, std::min(0.90, entry_price + 0.08), 0.50, false});
        levels.push_back({1, std::min(0.92, entry_price + 0.15), 1.00, false});
        spdlog::debug("Trend TP levels for entry={:.3f}: TP0={:.3f}(50%) TP1={:.3f}(rest)",
                      entry_price, levels[0].trigger_price, levels[1].trigger_price);
        return levels;
    }

    // TP0：45¢ → 卖出30%（锁住中间浮盈，防止涨后全回吐）
    levels.push_back({0, 0.45, 0.30, false});

    // TP1：70¢ → 卖出剩余的 ~43%（即原始的 30%）
    levels.push_back({1, 0.70, 3.0 / 7.0, false});

    // TP2：90¢ → 全部卖出
    levels.push_back({2, 0.90, 1.00, false});

    spdlog::debug("TP levels for entry={:.3f}: TP0=0.450(30%) TP1=0.700(30%) TP2=0.900(rest)",
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
    double mfe = pos.max_price - pos.entry_price;

    if (pos.regime == StrategyRegime::TREND) {
        if (current_contract_price <= pos.entry_price - 0.10) {
            exit.should_exit = true;
            exit.reason = "stop_price";
            exit.exit_price = current_contract_price;
            exit.use_market_order = true;
            spdlog::warn("TREND STOP: {} entry={:.3f} now={:.3f}",
                         pos.market_question, pos.entry_price, current_contract_price);
            return exit;
        }

        bool dev_crossed_zero =
            (pos.side == Side::UP && btc.deviation_pct <= 0) ||
            (pos.side == Side::DOWN && btc.deviation_pct >= 0);
        if (dev_crossed_zero) {
            exit.should_exit = true;
            exit.reason = "stop_btc";
            exit.exit_price = current_contract_price;
            exit.use_market_order = true;
            spdlog::warn("TREND DEV CROSS: {} side={} dev={:+.3f}% entry={:.3f} now={:.3f}",
                         pos.market_question, pos.side == Side::UP ? "UP" : "DOWN",
                         btc.deviation_pct, pos.entry_price, current_contract_price);
            return exit;
        }

        if (elapsed_sec >= 300 && mfe < 0.03) {
            exit.should_exit = true;
            exit.reason = "dead_water_exit";
            exit.exit_price = current_contract_price;
            exit.use_market_order = true;
            spdlog::warn("TREND DEAD WATER: {} elapsed={}s mfe_gain={:+.3f} entry={:.3f} now={:.3f}",
                         pos.market_question, elapsed_sec, mfe, pos.entry_price, current_contract_price);
            return exit;
        }

        if (mfe >= 0.15) {
            double trailing_stop = std::max(pos.entry_price + 0.06, pos.max_price - 0.05);
            if (current_contract_price <= trailing_stop) {
                exit.should_exit = true;
                exit.reason = "trailing_stop";
                exit.exit_price = current_contract_price;
                exit.use_market_order = true;
                spdlog::warn("TREND TRAILING (peak-5c): {} entry={:.3f} peak={:.3f} now={:.3f} stop={:.3f}",
                             pos.market_question, pos.entry_price, pos.max_price,
                             current_contract_price, trailing_stop);
                return exit;
            }
        } else if (mfe >= 0.08) {
            double trailing_stop = std::max(pos.entry_price + 0.02, pos.max_price - 0.06);
            if (current_contract_price <= trailing_stop) {
                exit.should_exit = true;
                exit.reason = "trailing_stop";
                exit.exit_price = current_contract_price;
                exit.use_market_order = true;
                spdlog::warn("TREND TRAILING (peak-6c): {} entry={:.3f} peak={:.3f} now={:.3f} stop={:.3f}",
                             pos.market_question, pos.entry_price, pos.max_price,
                             current_contract_price, trailing_stop);
                return exit;
            }
        }

        if (minutes_remaining <= 10) {
            return evaluate_last_10min(pos, current_contract_price, minutes_remaining);
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
    double mfe_gain = pos.max_price - pos.entry_price;
    double entry_dev = (pos.btc_price_at_entry - pos.btc_strike_at_entry)
        / pos.btc_strike_at_entry * 100.0;
    bool same_dev_side = (entry_dev >= 0 && btc.deviation_pct >= 0) ||
                         (entry_dev <= 0 && btc.deviation_pct <= 0);
    bool dev_expanded_again = same_dev_side &&
        std::abs(btc.deviation_pct) > std::abs(entry_dev) + 0.03;
    if (dev_expanded_again && current_contract_price <= pos.entry_price - 0.07) {
        exit.should_exit = true;
        exit.reason = "stop_btc";
        exit.exit_price = current_contract_price;
        exit.use_market_order = true;
        spdlog::warn("REVERSAL DEV EXPAND STOP: {} entry_dev={:+.3f}% now_dev={:+.3f}% entry={:.3f} now={:.3f}",
                     pos.market_question, entry_dev, btc.deviation_pct,
                     pos.entry_price, current_contract_price);
        return exit;
    }
    // Step 2.25：dead_water 加 price floor，避免亏损 -10~-30% 时被 dead_water 误退出
    //   实测 5 笔 dead_water 中 2 笔触发时 current 已 -37/-38%，这种深亏应让
    //   stop_price 接管而非 dead_water"接受任意亏损"。floor=entry × 0.85（亏 ≤15%）
    bool shallow_loss = (current_contract_price >= pos.entry_price * 0.85);
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
    if (mfe >= 0.15) {
        // MFE ≥ +15¢：peak-based 止损 = max(peak - 15¢, entry + 10¢)
        // 回吐超过峰值 15¢ 即离场；同时保底至少 +10¢ 利润
        double trailing_stop = std::max(pos.max_price - 0.15, pos.entry_price + 0.10);
        if (current_contract_price <= trailing_stop) {
            exit.should_exit = true;
            exit.reason = "trailing_stop";
            exit.exit_price = current_contract_price;
            exit.use_market_order = true;
            spdlog::warn("TRAILING STOP (peak-15c): {} entry={:.3f} peak={:.3f} now={:.3f} stop={:.3f}",
                         pos.market_question, pos.entry_price, pos.max_price,
                         current_contract_price, trailing_stop);
            return exit;
        }
    } else if (mfe >= 0.10) {
        // MFE ≥ +10¢：锁住 +5¢ 利润（Step 2.22 从保本档升级到 +5¢ 档）
        // 依据：4-29~4-30 三笔 trailing 亏损 max 都在 +11¢~+12¢，触发保本档后回吐到 entry-2¢
        // 出场（合计 -$1.40）；改为 entry+5¢ 阈值后这三笔可锁住 +$1.50（净 +$2.90）
        double trailing_stop = pos.entry_price + 0.05;
        if (current_contract_price <= trailing_stop) {
            exit.should_exit = true;
            exit.reason = "trailing_stop";
            exit.exit_price = current_contract_price;
            exit.use_market_order = true;
            spdlog::warn("TRAILING STOP (entry+5c): {} entry={:.3f} peak={:.3f} now={:.3f} stop={:.3f}",
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
    if (current_contract_price < 0.20) {
        // 0-20¢：立即清仓（时间止损）
        exit.should_exit = true;
        exit.reason = "stop_time";
        exit.exit_price = current_contract_price;
        exit.use_market_order = true;
        spdlog::warn("TIME STOP: price={:.3f} < 20¢ with {}min left",
                     current_contract_price, minutes_remaining);
    } else if (current_contract_price >= 0.80) {
        // 80¢以上：持有到期博 $1 结算
        spdlog::debug("HOLD TO EXPIRY: price={:.3f} >= 80¢, {}min left",
                      current_contract_price, minutes_remaining);
    }
    // 20-80¢：继续按止盈规则走

    return exit;
}

}  // namespace polymarket
