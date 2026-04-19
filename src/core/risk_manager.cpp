#include "core/risk_manager.h"

#include <spdlog/spdlog.h>

namespace polymarket {

RiskManager::RiskManager(const AppConfig& cfg)
    : account_balance_(cfg.strategy.account_balance)
    , fixed_shares_(5.0)           // 固定 5 shares
    , max_concurrent_(1)           // 单仓制：最多 1 个持仓
    , max_consecutive_losses_(3)   // 连续亏损 3 次停止
    , max_daily_drawdown_(0.05)    // 日回撤 5%
{}

bool RiskManager::can_open_position(const EntrySignal& sig,
                                     std::string& reject_reason) const {
    // §九.3 连续亏损3次，停止当日交易
    if (killed_) {
        reject_reason = "daily_kill_switch";
        return false;
    }

    if (consecutive_losses_ >= max_consecutive_losses_) {
        reject_reason = "consecutive_losses: " + std::to_string(consecutive_losses_);
        return false;
    }

    // §九.4 单日回撤超过5%
    double drawdown = -daily_pnl_ / account_balance_;
    if (drawdown >= max_daily_drawdown_) {
        reject_reason = "daily_drawdown: " + std::to_string(drawdown * 100) + "%";
        return false;
    }

    // §三 同时持仓上限
    if (open_positions_ >= max_concurrent_) {
        reject_reason = "max_concurrent: " + std::to_string(open_positions_) +
                        "/" + std::to_string(max_concurrent_);
        return false;
    }

    return true;
}

double RiskManager::compute_position_size() const {
    // §三 固定 5 shares，返回的是 shares 数量（不再是 USDC 金额）
    return fixed_shares_;
}

void RiskManager::record_loss(double amount) {
    daily_pnl_ -= amount;
    consecutive_losses_++;
    daily_trades_++;

    spdlog::info("RISK: loss ${:.2f} | daily P&L: ${:.2f} | consecutive losses: {}",
                 amount, daily_pnl_, consecutive_losses_);

    // §九.3
    if (consecutive_losses_ >= max_consecutive_losses_) {
        killed_ = true;
        spdlog::warn("RED LINE: {} consecutive losses, stopping for today",
                     consecutive_losses_);
    }

    // §九.4
    double drawdown = -daily_pnl_ / account_balance_;
    if (drawdown >= max_daily_drawdown_) {
        killed_ = true;
        spdlog::warn("RED LINE: daily drawdown {:.1f}% >= {:.1f}%, stopping for today",
                     drawdown * 100, max_daily_drawdown_ * 100);
    }
}

void RiskManager::record_profit(double amount) {
    daily_pnl_ += amount;
    consecutive_losses_ = 0;  // 重置连续亏损计数
    daily_trades_++;

    spdlog::info("RISK: profit ${:.2f} | daily P&L: ${:.2f} | streak reset",
                 amount, daily_pnl_);
}

void RiskManager::reset_daily() {
    consecutive_losses_ = 0;
    daily_pnl_ = 0;
    daily_trades_ = 0;
    killed_ = false;
    spdlog::info("RISK: daily stats reset");
}

}  // namespace polymarket
