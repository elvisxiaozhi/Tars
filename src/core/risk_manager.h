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

    // 计算下单数量（shares）
    double compute_position_size() const;

    // 记录一次亏损
    void record_loss(double amount);

    // 记录一次盈利
    void record_profit(double amount);

    // 标记本场（本小时 K线）已止损，禁止再交易
    void set_candle_stopped() { candle_stopped_ = true; }
    void set_candle_stopped(const std::string&) { set_candle_stopped(); }
    bool is_candle_stopped() const { return candle_stopped_; }

    // 新 K线 开始时重置
    void reset_candle() { candle_stopped_ = false; }
    void reset_candle(const std::string&) { reset_candle(); }
    void reset_global_hour() {}

    // 重置每日统计
    void reset_daily();

    // 账户余额
    double account_balance() const { return account_balance_; }
    void deduct_balance(double amount) { account_balance_ -= amount; }
    void add_balance(double amount) { account_balance_ += amount; }

    // Getters
    int consecutive_losses() const { return consecutive_losses_; }
    int consecutive_wins() const { return consecutive_wins_; }
    double daily_pnl() const { return daily_pnl_; }
    int open_position_count() const { return open_positions_; }

    // 仓位计数
    void add_position() { open_positions_++; }
    void add_position(const std::string&, StrategyRegime) { add_position(); }
    void remove_position() { if (open_positions_ > 0) open_positions_--; }

private:
    bool live_mode_;
    double account_balance_;
    double fixed_shares_;          // 固定下单量 5 shares
    int max_concurrent_;           // 最多同时 1 个（单仓制）

    int open_positions_ = 0;
    int consecutive_losses_ = 0;
    int consecutive_wins_ = 0;
    int daily_trades_ = 0;
    double daily_pnl_ = 0;
    bool candle_stopped_ = false;  // 本场 K线 已止损，禁止再交易
};

}  // namespace polymarket
