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

    // §一.1 时间窗口：剩余时间 > 20 分钟
    // 从 30→20 的依据：4-26 早间数据显示 BTC 波动集中在蜡烛末段，30 阈值屏蔽了 100% 的甜区机会
    // 20 min 仍能承接 TP0（contract +20¢ 在 BTC 走方向时 5-10min 可达），但 TP1/TP2 命中率会降
    if (minutes_remaining <= 20) {
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

    // §五.1b 死水早退：入场 8 分钟后若 MFE 未启动，立即平仓
    // Step 2.23：阈值从 5¢ 收紧到 2¢（dry 14 笔回放只让 3 笔深死 case 触发）。
    // Step 2.24：窗口从 5min → 8min（实验性单变量调整，观察 5-10 笔再评估）。
    //   触发：5/1 live 4/4 都是"近平卖飞"，无救命 case。两笔反弹时刻：
    //     · 18:10 case：7m00s 涨到 0.43（mfe +20¢） → 8min 窗口能救
    //     · 13:11 case：18+ min 才反弹 → 8min 仍救不了，接受 -$0.36 损失
    //   救命 case（04-29、04-30 15:25）多扛 3 分钟，最坏多损 ~$0.2/笔。
    auto now_chrono = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    int64_t elapsed_sec = (now_chrono - pos.entry_time) / 1000;
    double mfe_gain = pos.max_price - pos.entry_price;
    if (elapsed_sec >= 480 && mfe_gain < 0.02) {
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
