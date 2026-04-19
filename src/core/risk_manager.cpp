#include "core/risk_manager.h"

#include <spdlog/spdlog.h>

namespace polymarket {

RiskManager::RiskManager(const AppConfig& cfg)
    : account_balance_(cfg.strategy.account_balance)
    , fixed_shares_(5.0)           // 固定 5 shares
    , max_concurrent_(1)           // 单仓制：最多 1 个持仓
{}

bool RiskManager::can_open_position(const EntrySignal& sig,
                                     std::string& reject_reason) const {
    // §三 本场止损后禁止再交易
    if (candle_stopped_) {
        reject_reason = "candle_stopped: already stop-lossed this candle";
        return false;
    }

    // §三 同时持仓上限（单仓制）
    if (open_positions_ >= max_concurrent_) {
        reject_reason = "max_concurrent: " + std::to_string(open_positions_) +
                        "/" + std::to_string(max_concurrent_);
        return false;
    }

    return true;
}

double RiskManager::compute_position_size() const {
    // §三 固定 5 shares
    return fixed_shares_;
}

void RiskManager::record_loss(double amount) {
    daily_pnl_ -= amount;
    consecutive_losses_++;
    daily_trades_++;

    spdlog::info("RISK: loss ${:.2f} | daily P&L: ${:.2f} | consecutive losses: {}",
                 amount, daily_pnl_, consecutive_losses_);
}

void RiskManager::record_profit(double amount) {
    daily_pnl_ += amount;
    consecutive_losses_ = 0;
    daily_trades_++;

    spdlog::info("RISK: profit ${:.2f} | daily P&L: ${:.2f} | streak reset",
                 amount, daily_pnl_);
}

void RiskManager::reset_daily() {
    consecutive_losses_ = 0;
    daily_pnl_ = 0;
    daily_trades_ = 0;
    candle_stopped_ = false;
    spdlog::info("RISK: daily stats reset");
}

}  // namespace polymarket
