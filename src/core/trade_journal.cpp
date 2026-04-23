#include "core/trade_journal.h"

#include <filesystem>
#include <map>

#include <json.hpp>
#include <spdlog/spdlog.h>

namespace polymarket {

using json = nlohmann::json;

// 从交易 ID 中提取仓位基础 ID（去掉 "-TP1"/"-TP2" 后缀）
static std::string base_position_id(const std::string& id) {
    auto pos = id.find("-TP");
    return pos != std::string::npos ? id.substr(0, pos) : id;
}

TradeJournal::TradeJournal(const std::string& path) : path_(path) {
    // 确保目录存在
    auto dir = std::filesystem::path(path).parent_path();
    if (!dir.empty()) {
        std::filesystem::create_directories(dir);
    }
}

void TradeJournal::record(const TradeRecord& trade) {
    std::lock_guard<std::mutex> lock(mu_);
    records_.push_back(trade);

    // 追加写入 JSONL 文件
    json j;
    j["id"] = trade.id;
    j["market"] = trade.market_question;
    j["side"] = trade.side;
    j["entry_time"] = trade.entry_time;
    j["exit_time"] = trade.exit_time;
    j["minutes_remaining"] = trade.minutes_remaining_at_entry;
    j["entry_price"] = trade.entry_price;
    j["exit_price"] = trade.exit_price;
    j["size_usdc"] = trade.size_usdc;
    j["shares"] = trade.shares;
    j["btc_price"] = trade.btc_price_at_entry;
    j["btc_strike"] = trade.btc_strike;
    j["btc_deviation_pct"] = trade.btc_deviation_pct;
    j["entry_vol"] = trade.entry_vol;
    j["avg_vol"] = trade.avg_vol;
    j["exit_reason"] = trade.exit_reason;
    j["pnl"] = trade.realized_pnl;
    j["fee"] = trade.fee_paid;

    // Analytics
    j["max_price"] = trade.max_price;
    j["min_price"] = trade.min_price;
    j["btc_price_at_exit"] = trade.btc_price_at_exit;
    j["btc_deviation_at_exit"] = trade.btc_deviation_at_exit;
    j["spread_at_entry"] = trade.spread_at_entry;
    j["ask_depth_at_entry"] = trade.ask_depth_at_entry;
    j["hour_et"] = trade.hour_et;
    j["day_of_week"] = trade.day_of_week;
    j["consec_wins_before"] = trade.consec_wins_before;
    j["consec_losses_before"] = trade.consec_losses_before;
    j["balance_before"] = trade.balance_before;
    j["hold_duration_sec"] = trade.hold_duration_sec;
    j["mfe_capture_rate"] = trade.mfe_capture_rate;

    std::ofstream f(path_, std::ios::app);
    if (f.is_open()) {
        f << j.dump() << "\n";
    }
}

void TradeJournal::print_summary() const {
    if (records_.empty()) {
        spdlog::info("Trade journal: no trades recorded");
        return;
    }

    spdlog::info("");
    spdlog::info("=== Trade Summary ({} candles, {} records) ===", candle_count(), records_.size());
    spdlog::info("{:<45} {:>5} {:>8} {:>8} {:>8} {:>10}",
                 "Market", "Side", "Entry", "Exit", "Reason", "P&L");
    spdlog::info("{}", std::string(90, '-'));

    for (const auto& t : records_) {
        std::string q = t.market_question;
        if (q.size() > 45) q = q.substr(0, 42) + "...";
        spdlog::info("{:<45} {:>5} {:>8.3f} {:>8.3f} {:>8} {:>+10.2f}",
                     q, t.side, t.entry_price, t.exit_price,
                     t.exit_reason, t.realized_pnl);
    }

    spdlog::info("{}", std::string(90, '-'));
    spdlog::info("Total P&L: ${:+.2f} | Win rate: {:.1f}% ({}/{})",
                 total_pnl(), candle_win_rate() * 100, candle_wins(), candle_count());
}

// 按仓位 ID 分组，汇总每个仓位的总 P&L
static std::map<std::string, double> group_pnl_by_position(const std::vector<TradeRecord>& records) {
    std::map<std::string, double> grouped;
    for (const auto& t : records) {
        grouped[base_position_id(t.id)] += t.realized_pnl;
    }
    return grouped;
}

int TradeJournal::candle_count() const {
    return static_cast<int>(group_pnl_by_position(records_).size());
}

int TradeJournal::candle_wins() const {
    int w = 0;
    for (const auto& [id, pnl] : group_pnl_by_position(records_)) {
        if (pnl > 0) w++;
    }
    return w;
}

int TradeJournal::candle_losses() const {
    int l = 0;
    for (const auto& [id, pnl] : group_pnl_by_position(records_)) {
        if (pnl <= 0) l++;
    }
    return l;
}

double TradeJournal::candle_win_rate() const {
    int count = candle_count();
    if (count == 0) return 0;
    return static_cast<double>(candle_wins()) / count;
}

double TradeJournal::total_pnl() const {
    double sum = 0;
    for (const auto& t : records_) sum += t.realized_pnl;
    return sum;
}

}  // namespace polymarket
