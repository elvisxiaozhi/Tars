#include "core/risk_manager.h"

#include <spdlog/spdlog.h>

namespace polymarket {

RiskManager::RiskManager(const AppConfig& cfg)
    : live_mode_(cfg.strategy.mode == "live")
    , account_balance_(cfg.strategy.account_balance)
    , fixed_shares_(10.0)          // 固定 10 shares：30/30/40 切成整数 3/3/4，配市价出场满足 Polymarket 最小订单约束
    , max_concurrent_(1)           // 单仓制：最多 1 个持仓
{}

bool RiskManager::can_open_position(const EntrySignal& sig,
                                     std::string& reject_reason) const {
    // A stop exit pauses only that coin's current candle. Dry run still needs
    // this gate to avoid repeatedly sampling the same failed setup.
    if (candle_stopped_ || stopped_coins_.count(sig.coin) > 0) {
        reject_reason = "candle_stopped: already stop-lossed this candle";
        return false;
    }

    // Live mode keeps the original global single-position risk gate.
    if (live_mode_ && open_positions_ >= max_concurrent_) {
        reject_reason = "max_concurrent: " + std::to_string(open_positions_) +
                        "/" + std::to_string(max_concurrent_);
        return false;
    }

    // 余额检查：new low-cost branch sizes each trade at roughly $1.
    double cost = sig.size_usdc > 0 ? sig.size_usdc : fixed_shares_ * sig.entry_price;
    if (live_mode_ && cost > account_balance_) {
        reject_reason = "insufficient_balance: need $" + std::to_string(cost) +
                        " but have $" + std::to_string(account_balance_);
        return false;
    }

    return true;
}

void RiskManager::set_candle_stopped(const std::string& coin) {
    if (coin.empty()) {
        candle_stopped_ = true;
        return;
    }
    stopped_coins_.insert(coin);
}

bool RiskManager::is_candle_stopped(const std::string& coin) const {
    return candle_stopped_ || stopped_coins_.count(coin) > 0;
}

void RiskManager::reset_candle(const std::string& coin) {
    if (coin.empty()) {
        reset_candle();
        return;
    }
    stopped_coins_.erase(coin);
}

void RiskManager::reset_global_hour() {
    candle_stopped_ = false;
    stopped_coins_.clear();
}

double RiskManager::compute_position_size() const {
    // Kept for compatibility; entry sizing now uses EntrySignal::shares.
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
    stopped_coins_.clear();
    spdlog::info("RISK: daily stats reset");
}

}  // namespace polymarket
