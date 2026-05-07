#pragma once

#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "core/strategy.h"

namespace polymarket {

struct TradeRecord {
    std::string id;
    std::string mode;            // "live" / "dry_run"（区分实盘 vs 模拟；老记录加载时默认 "dry_run"）
    std::string coin = "BTC";
    std::string regime;
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
    // 各时间点 MFE_gain 占 entry 的比例（用于评估「死水阈值是否该按比例化」）
    double mfe5_gain_pct = 0;   // (mfe_at_5min - entry) / entry
    double mfe10_gain_pct = 0;
    double mfe15_gain_pct = 0;
    bool has_tp_before_exit = false;  // 本条记录生成前/生成时该仓位是否已有 TP 触发
    int tp_count_before_exit = 0;     // 本条记录生成前/生成时该仓位已触发 TP 数量
    // §五.1b dead_water 触发时的市场上下文埋点（仅 exit_reason=="dead_water_exit" 填）
    // Step 2.23：用于 20+ 笔后回归是否要加 BTC 方向 / vol 条件
    double dw_btc_dev_pct = 0;       // 触发瞬间 BTC dev (%)
    double dw_vol_ratio = 0;         // current_1h_vol / avg_24h_vol（>1 = 当前更活跃）
    double dw_spread = 0;            // 触发瞬间 ask-bid（我方 token）
    bool   dw_dev_favors = false;    // BTC dev 方向是否帮助仓位（UP & dev>0 / DOWN & dev<0）
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
    size_t session_start_index_ = 0;  // 本次会话开始时的 records_.size()，print_summary 据此区分新旧
    std::mutex mu_;
};

}  // namespace polymarket
