#include "core/risk_manager.h"

#include <spdlog/spdlog.h>

namespace polymarket {

RiskManager::RiskManager(const AppConfig& cfg)
    : account_balance_(cfg.risk.max_total_exposure)  // 用 max_total_exposure 作为账户余额
    , position_pct_(0.02)          // 单份 2%
    , max_position_pct_(0.10)      // 硬性上限 10%
    , max_concurrent_(2)           // 最多 2 个同时持仓
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
    // §三 单份 = 账户总资金的 1-2%
    double size = account_balance_ * position_pct_;

    // 硬性上限: 不超过 max_single_trade
    if (size > account_balance_ * max_position_pct_) {
        size = account_balance_ * max_position_pct_;
    }

    // 最小 $10（实测阶段）
    if (size < 10.0) size = 10.0;

    return size;
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
