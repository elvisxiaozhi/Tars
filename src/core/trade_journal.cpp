#include "core/trade_journal.h"

#include <filesystem>

#include <json.hpp>
#include <spdlog/spdlog.h>

namespace polymarket {

using json = nlohmann::json;

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
    spdlog::info("=== Trade Summary ({} trades) ===", records_.size());
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
                 total_pnl(), win_rate() * 100, wins(), total_trades());
}

int TradeJournal::wins() const {
    int w = 0;
    for (const auto& t : records_) {
        if (t.realized_pnl > 0) w++;
    }
    return w;
}

int TradeJournal::losses() const {
    int l = 0;
    for (const auto& t : records_) {
        if (t.realized_pnl <= 0) l++;
    }
    return l;
}

double TradeJournal::total_pnl() const {
    double sum = 0;
    for (const auto& t : records_) sum += t.realized_pnl;
    return sum;
}

double TradeJournal::win_rate() const {
    if (records_.empty()) return 0;
    return static_cast<double>(wins()) / records_.size();
}

}  // namespace polymarket
