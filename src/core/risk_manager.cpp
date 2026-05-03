#include "core/risk_manager.h"

#include <spdlog/spdlog.h>

namespace polymarket {

RiskManager::RiskManager(const AppConfig& cfg)
    : cfg_(cfg)
    , account_balance_(cfg.strategy.account_balance)
    , fixed_shares_(10.0)          // 固定 10 shares：30/30/40 切成整数 3/3/4，配市价出场满足 Polymarket 最小订单约束
    , max_concurrent_(cfg.strategy.max_global_open_positions)
{}

bool RiskManager::can_open_position(const EntrySignal& sig,
                                     std::string& reject_reason) const {
    const auto state_it = coin_state_.find(sig.coin);
    CoinRiskState cs = state_it != coin_state_.end() ? state_it->second : CoinRiskState{};

    if (cs.candle_stopped) {
        reject_reason = "candle_stopped: " + sig.coin + " already stopped this candle";
        return false;
    }

    if (cs.candle_traded) {
        reject_reason = "candle_traded: " + sig.coin + " one entry already used this candle";
        return false;
    }

    if (open_positions_ >= max_concurrent_) {
        reject_reason = "max_concurrent: " + std::to_string(open_positions_) +
                        "/" + std::to_string(max_concurrent_);
        return false;
    }

    if (global_trades_this_hour_ >= cfg_.strategy.max_global_trades_per_hour) {
        reject_reason = "global_hour_trades: " + std::to_string(global_trades_this_hour_) +
                        "/" + std::to_string(cfg_.strategy.max_global_trades_per_hour);
        return false;
    }

    if (sig.regime == StrategyRegime::QUIET_REVERSION &&
        quiet_trades_this_hour_ >= cfg_.strategy.max_quiet_trades_per_hour) {
        reject_reason = "quiet_hour_trades: " + std::to_string(quiet_trades_this_hour_) +
                        "/" + std::to_string(cfg_.strategy.max_quiet_trades_per_hour);
        return false;
    }

    // 余额检查：regime strategy sets target USDC size dynamically.
    double cost = sig.size_usdc > 0 ? sig.size_usdc : fixed_shares_ * sig.entry_price;
    if (cost > account_balance_) {
        reject_reason = "insufficient_balance: need $" + std::to_string(cost) +
                        " but have $" + std::to_string(account_balance_);
        return false;
    }

    return true;
}

void RiskManager::reset_candle(const std::string& coin) {
    coin_state_[coin] = CoinRiskState{};
}

void RiskManager::reset_global_hour() {
    global_trades_this_hour_ = 0;
    quiet_trades_this_hour_ = 0;
}

void RiskManager::add_position(const std::string& coin, StrategyRegime regime) {
    open_positions_++;
    auto& cs = coin_state_[coin];
    cs.candle_traded = true;
    cs.trades_this_hour++;
    global_trades_this_hour_++;
    if (regime == StrategyRegime::QUIET_REVERSION) quiet_trades_this_hour_++;
}

double RiskManager::compute_position_size() const {
    // §三 固定 10 shares
    return fixed_shares_;
}

void RiskManager::record_loss(double amount) {
    daily_pnl_ -= amount;
    consecutive_losses_++;
    consecutive_wins_ = 0;
    daily_trades_++;

    spdlog::debug("RISK: loss ${:.2f} | daily_pnl=${:.2f} | consec_loss={}",
                  amount, daily_pnl_, consecutive_losses_);
}

void RiskManager::record_profit(double amount) {
    daily_pnl_ += amount;
    consecutive_losses_ = 0;
    consecutive_wins_++;
    daily_trades_++;

    spdlog::debug("RISK: profit ${:.2f} | daily_pnl=${:.2f}",
                  amount, daily_pnl_);
}

void RiskManager::reset_daily() {
    consecutive_losses_ = 0;
    consecutive_wins_ = 0;
    daily_pnl_ = 0;
    daily_trades_ = 0;
    candle_stopped_ = false;
    coin_state_.clear();
    global_trades_this_hour_ = 0;
    quiet_trades_this_hour_ = 0;
    spdlog::info("RISK: daily stats reset");
}

}  // namespace polymarket
