#pragma once

#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "core/strategy.h"

namespace polymarket {

struct TradeRecord {
    std::string id;
    std::string market_question;
    std::string side;            // "UP" / "DOWN"
    int64_t entry_time = 0;
    int64_t exit_time = 0;
    int minutes_remaining_at_entry = 0;
    double entry_price = 0;
    double exit_price = 0;
    double size_usdc = 0;
    double shares = 0;
    double btc_price_at_entry = 0;
    double btc_strike = 0;
    double btc_deviation_pct = 0;
    double entry_vol = 0;
    double avg_vol = 0;
    std::string exit_reason;     // "tp0"/"tp1"/"tp2"/"trailing_stop"/"stop_price"/"stop_time"/"expired"
    double realized_pnl = 0;
    double fee_paid = 0;

    // Analytics
    double max_price = 0;              // MFE: 持仓期间最高价
    double min_price = 0;              // MAE: 持仓期间最低价
    double btc_price_at_exit = 0;
    double btc_deviation_at_exit = 0;  // (btc_exit - strike) / strike * 100
    double spread_at_entry = 0;
    double ask_depth_at_entry = 0;
    int hour_et = -1;
    int day_of_week = -1;
    int consec_wins_before = 0;
    int consec_losses_before = 0;
    double balance_before = 0;
    int hold_duration_sec = 0;
    double mfe_capture_rate = 0;   // (exit-entry)/(max-entry)，出场效率
    // 入场后 N 分钟时间窗口内的最高价（观测字段，用于死水早退规则评估）
    double mfe_at_5min = 0;
    double mfe_at_10min = 0;
    double mfe_at_15min = 0;
};

class TradeJournal {
public:
    explicit TradeJournal(const std::string& path = "./logs/trades.jsonl");

    // 记录一笔交易
    void record(const TradeRecord& trade);

    // 打印当日摘要
    void print_summary() const;

    // 获取所有记录
    const std::vector<TradeRecord>& records() const { return records_; }

    int total_records() const { return static_cast<int>(records_.size()); }

    // K 线级统计（按仓位分组，同一仓位的 TP 子记录 + 平仓记录汇总判断盈亏）
    int candle_count() const;
    int candle_wins() const;
    int candle_losses() const;
    double candle_win_rate() const;

    // 总 P&L（每条记录各自的 pnl 之和，无重复计算）
    double total_pnl() const;

private:
    std::string path_;
    std::vector<TradeRecord> records_;
    std::mutex mu_;
};

}  // namespace polymarket
