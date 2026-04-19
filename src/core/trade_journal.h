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
    std::string exit_reason;     // "tp1"/"tp2"/"stop_price"/"stop_btc"/"stop_time"/"expired"
    double realized_pnl = 0;
    double fee_paid = 0;
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

    int total_trades() const { return static_cast<int>(records_.size()); }
    int wins() const;
    int losses() const;
    double total_pnl() const;
    double win_rate() const;

private:
    std::string path_;
    std::vector<TradeRecord> records_;
    std::mutex mu_;
};

}  // namespace polymarket
