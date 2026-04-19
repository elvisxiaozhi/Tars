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
    // §九.3 连亏熔断：dry_run 阶段暂时禁用，live 模式需重新启用
    // §九.4 日亏熔断：dry_run 阶段暂时禁用，live 模式需重新启用

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

    // dry_run 阶段不触发熔断，仅记录日志
    // TODO(live): 重新启用连亏熔断和日亏熔断
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
