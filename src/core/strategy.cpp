#include "core/strategy.h"

#include <cmath>

#include <spdlog/spdlog.h>

namespace polymarket {

Strategy::Strategy(const AppConfig& cfg) : cfg_(cfg) {}

double Strategy::max_entry_price(int minutes_remaining) const {
    // §一：剩余30分钟以上，ask < 30¢
    if (minutes_remaining > 30) return 0.30;
    return 0;  // 不入场
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

    // §一.1 时间窗口：剩余时间 > 30分钟
    if (minutes_remaining <= 30) {
        sig.reject_reason = "time_too_short: " + std::to_string(minutes_remaining) + "min left";
        return sig;
    }

    double max_price = max_entry_price(minutes_remaining);
    if (max_price <= 0) {
        sig.reject_reason = "no_entry_window";
        return sig;
    }

    // 方向选择：选择更便宜的一方买入
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

    // 价格条件
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
    // §二：在当前价下方1¢挂限价买单
    sig.entry_price = candidate_ask - 0.01;
    if (sig.entry_price < 0.01) sig.entry_price = 0.01;
    sig.token_id = candidate_token;

    spdlog::info("SIGNAL: {} {} @ {:.3f} (ask={:.3f}, max={:.3f}, BTC dev={:+.2f}%)",
                 (sig.side == Side::UP ? "UP" : "DOWN"),
                 question, sig.entry_price, candidate_ask, max_price,
                 btc.deviation_pct);

    return sig;
}

std::vector<TakeProfitLevel> Strategy::compute_tp_levels(double entry_price) {
    // §四 止盈规则（三档减仓，中间档锁住 MFE 浮盈）
    std::vector<TakeProfitLevel> levels;

    // TP0：45¢ → 卖出30%（锁住中间浮盈，防止涨后全回吐）
    levels.push_back({0, 0.45, 0.30, false});

    // TP1：70¢ → 卖出剩余的 ~43%（即原始的 30%）
    levels.push_back({1, 0.70, 3.0 / 7.0, false});

    // TP2：90¢ → 全部卖出
    levels.push_back({2, 0.90, 1.00, false});

    spdlog::info("TP levels for entry={:.3f}: TP0=0.450(30%) TP1=0.700(30%) TP2=0.900(rest)",
                 entry_price);

    return levels;
}

ExitSignal Strategy::evaluate_exit(
    const Position& pos,
    double current_contract_price,
    const BtcMarketData& btc,
    int minutes_remaining) {

    ExitSignal exit;

    // §五.1 价格止损：从入场价下跌 ≥ 50%（优先于时间止损，防止跳空超额亏损）
    double loss_pct = (pos.entry_price - current_contract_price) / pos.entry_price;
    if (loss_pct >= 0.50) {
        exit.should_exit = true;
        exit.reason = "stop_price";
        exit.exit_price = current_contract_price;
        exit.use_market_order = true;
        spdlog::warn("STOP LOSS (price): {} loss={:.1f}% entry={:.3f} now={:.3f}",
                     pos.market_question, loss_pct * 100,
                     pos.entry_price, current_contract_price);
        return exit;
    }

    // §五.2 移动止盈（Trailing Stop）：基于 MFE 动态提升止损线
    double mfe = pos.max_price - pos.entry_price;
    if (mfe >= 0.25) {
        // MFE ≥ +25¢：锁定 +10¢ 利润
        double trailing_stop = pos.entry_price + 0.10;
        if (current_contract_price <= trailing_stop) {
            exit.should_exit = true;
            exit.reason = "trailing_stop";
            exit.exit_price = current_contract_price;
            exit.use_market_order = true;
            spdlog::warn("TRAILING STOP (lock +10c): {} entry={:.3f} peak={:.3f} now={:.3f} stop={:.3f}",
                         pos.market_question, pos.entry_price, pos.max_price,
                         current_contract_price, trailing_stop);
            return exit;
        }
    } else if (mfe >= 0.15) {
        // MFE ≥ +15¢：保本止损
        if (current_contract_price <= pos.entry_price) {
            exit.should_exit = true;
            exit.reason = "trailing_stop";
            exit.exit_price = current_contract_price;
            exit.use_market_order = true;
            spdlog::warn("TRAILING STOP (breakeven): {} entry={:.3f} peak={:.3f} now={:.3f}",
                         pos.market_question, pos.entry_price, pos.max_price,
                         current_contract_price);
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
        spdlog::info("HOLD TO EXPIRY: price={:.3f} >= 80¢, {}min left",
                     current_contract_price, minutes_remaining);
    }
    // 20-80¢：继续按止盈规则走

    return exit;
}

}  // namespace polymarket
