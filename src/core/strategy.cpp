#include "core/strategy.h"

#include <cmath>

#include <spdlog/spdlog.h>

namespace polymarket {

Strategy::Strategy(const AppConfig& cfg) : cfg_(cfg) {}

double Strategy::max_entry_price(int minutes_remaining) const {
    // 策略规则 §一：剩余30分钟以上，ask < 30¢
    if (minutes_remaining > 30) return 0.30;
    return 0;  // 不入场
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

    // §一.1 时间窗口：剩余时间 > 30分钟
    if (minutes_remaining <= 30) {
        sig.reject_reason = "time_too_short: " + std::to_string(minutes_remaining) + "min left";
        return sig;
    }

    // §一.3 波动率条件：已移除（K线初期波动率天然为0，与早期入场窗口矛盾）

    double max_price = max_entry_price(minutes_remaining);
    if (max_price <= 0) {
        sig.reject_reason = "no_entry_window";
        return sig;
    }

    // §一.4 方向选择：选择更便宜的一方买入
    Side candidate_side = Side::NONE;
    double candidate_ask = 0;
    std::string candidate_token;

    if (up_ask > 0 && (down_ask <= 0 || up_ask <= down_ask)) {
        candidate_side = Side::UP;
        candidate_ask = up_ask;
        candidate_token = up_token_id;
    } else if (down_ask > 0) {
        candidate_side = Side::DOWN;
        candidate_ask = down_ask;
        candidate_token = down_token_id;
    } else {
        sig.reject_reason = "no_valid_ask";
        return sig;
    }

    // §一.2 价格条件
    if (candidate_ask <= 0 || candidate_ask >= 1.0) {
        sig.reject_reason = "invalid_ask: " + std::to_string(candidate_ask);
        return sig;
    }

    if (candidate_ask > max_price) {
        sig.reject_reason = "ask_too_high: " +
            std::to_string(candidate_ask) + " > max " + std::to_string(max_price);
        return sig;
    }

    // §九.1 红线：不追涨买入超过30¢的合约
    if (candidate_ask > 0.30) {
        sig.reject_reason = "red_line_30c: ask=" + std::to_string(candidate_ask);
        return sig;
    }

    // 信号有效
    sig.valid = true;
    sig.side = candidate_side;
    sig.market_ask = candidate_ask;
    // §二：在当前价下方0.5-1¢挂限价买单
    sig.entry_price = candidate_ask - 0.01;
    if (sig.entry_price < 0.01) sig.entry_price = 0.01;
    sig.token_id = candidate_token;

    spdlog::info("SIGNAL: {} {} @ {:.3f} (ask={:.3f}, max={:.3f}, BTC dev={:+.2f}%, vol={:.4f})",
                 (sig.side == Side::UP ? "UP" : "DOWN"),
                 question, sig.entry_price, candidate_ask, max_price,
                 btc.deviation_pct, btc.current_1h_vol);

    return sig;
}

std::vector<TakeProfitLevel> Strategy::compute_tp_levels(double entry_price) {
    // §四 止盈规则（分档减仓，让利润充分奔跑）
    std::vector<TakeProfitLevel> levels;

    // 第一档：1.5 × P₀ → 卖出30%（让利润跑一会再锁定）
    levels.push_back({1, entry_price * 1.5, 0.30, false});

    // 第二档：价格翻倍 2 × P₀ → 卖出30%
    levels.push_back({2, entry_price * 2.0, 0.30, false});

    // 第三档：75¢ → 卖出20%
    levels.push_back({3, 0.75, 0.20, false});

    // 第四档：85¢ → 持有到期（剩余20%博 $1 结算）
    levels.push_back({4, 0.85, 0.20, false});

    return levels;
}

ExitSignal Strategy::evaluate_exit(
    const Position& pos,
    double current_contract_price,
    const BtcMarketData& btc,
    int minutes_remaining) {

    ExitSignal exit;

    // §六 最后10分钟特殊处理
    if (minutes_remaining <= 10) {
        return evaluate_last_10min(pos, current_contract_price, minutes_remaining);
    }

    // §五.1 价格止损：从入场价下跌 ≥ 35%
    double loss_pct = (pos.entry_price - current_contract_price) / pos.entry_price;
    if (loss_pct >= 0.35) {
        exit.should_exit = true;
        exit.reason = "stop_price";
        exit.exit_price = current_contract_price;
        exit.use_market_order = true;  // 止损用市价单
        spdlog::warn("STOP LOSS (price): {} loss={:.1f}% entry={:.3f} now={:.3f}",
                     pos.market_question, loss_pct * 100,
                     pos.entry_price, current_contract_price);
        return exit;
    }

    // §五.2 BTC价格止损
    // 做UP: BTC跌破入场时的 strike → 趋势反转
    // 做DOWN: BTC突破入场时的 strike → 趋势反转
    if (pos.side == Side::UP) {
        // 入场时 BTC 走弱（低于 strike），如果继续走弱（跌幅扩大到 > 0.5%），止损
        double btc_loss_from_strike = (pos.btc_strike_at_entry - btc.current_price) /
                                       pos.btc_strike_at_entry * 100.0;
        if (btc_loss_from_strike > 0.5) {
            exit.should_exit = true;
            exit.reason = "stop_btc";
            exit.exit_price = current_contract_price;
            exit.use_market_order = true;
            spdlog::warn("STOP LOSS (BTC): UP position, BTC fell {:.2f}% below strike",
                         btc_loss_from_strike);
            return exit;
        }
    } else if (pos.side == Side::DOWN) {
        double btc_gain_over_strike = (btc.current_price - pos.btc_strike_at_entry) /
                                       pos.btc_strike_at_entry * 100.0;
        if (btc_gain_over_strike > 0.5) {
            exit.should_exit = true;
            exit.reason = "stop_btc";
            exit.exit_price = current_contract_price;
            exit.use_market_order = true;
            spdlog::warn("STOP LOSS (BTC): DOWN position, BTC rose {:.2f}% above strike",
                         btc_gain_over_strike);
            return exit;
        }
    }

    // §五.3 时间止损：剩余 ≤ 10分钟 → 在 evaluate_last_10min 处理

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
        // 80¢以上：持有到期，取消卖出挂单
        spdlog::info("HOLD TO EXPIRY: price={:.3f} >= 80¢, {}min left",
                     current_contract_price, minutes_remaining);
        // 不退出，让到期结算
    } else if (current_contract_price >= 0.50) {
        // 50-80¢：继续按止盈规则走（不做特殊处理）
    } else {
        // 20-50¢：按止盈规则执行，但取消第三、第四档
        // （这里不强制退出，由 tp_levels 控制）
    }

    return exit;
}

}  // namespace polymarket
