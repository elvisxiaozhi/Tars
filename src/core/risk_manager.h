#pragma once

#include <string>
#include <vector>

#include "core/strategy.h"
#include "utils/config.h"

namespace polymarket {

class RiskManager {
public:
    explicit RiskManager(const AppConfig& cfg);

    // 检查是否允许开新仓
    bool can_open_position(const EntrySignal& sig, std::string& reject_reason) const;

    // 计算仓位大小（USDC）
    double compute_position_size() const;

    // 记录一次亏损
    void record_loss(double amount);

    // 记录一次盈利
    void record_profit(double amount);

    // 重置每日统计（新一天调用）
    void reset_daily();

    // Getters
    int consecutive_losses() const { return consecutive_losses_; }
    double daily_pnl() const { return daily_pnl_; }
    int open_position_count() const { return open_positions_; }
    bool is_killed() const { return killed_; }

    // 仓位计数
    void add_position() { open_positions_++; }
    void remove_position() { if (open_positions_ > 0) open_positions_--; }

private:
    double account_balance_;
    double fixed_shares_;          // 固定下单量 5 shares
    int max_concurrent_;           // 最多同时 1 个（单仓制）
    int max_consecutive_losses_;  // 连续亏损 3 次停止
    double max_daily_drawdown_;   // 日回撤 5%

    int open_positions_ = 0;
    int consecutive_losses_ = 0;
    int daily_trades_ = 0;
    double daily_pnl_ = 0;
    bool killed_ = false;         // 触发红线，当日停止
};

}  // namespace polymarket
