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

    // 启动加载历史 jsonl（容错：缺字段用默认；解析失败的行跳过）
    std::ifstream f(path_);
    if (!f.is_open()) return;

    std::string line;
    int loaded = 0, skipped = 0;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        try {
            auto j = json::parse(line);
            TradeRecord r;
            r.id                          = j.value("id", "");
            // mode 字段：老记录无此字段 → 默认 "dry_run"（4-28 V2 升级前全是模拟）
            r.mode                        = j.value("mode", std::string{"dry_run"});
            r.market_question             = j.value("market", "");
            r.side                        = j.value("side", "");
            r.entry_time                  = j.value("entry_time", int64_t{0});
            r.exit_time                   = j.value("exit_time", int64_t{0});
            r.minutes_remaining_at_entry  = j.value("minutes_remaining", 0);
            r.entry_price                 = j.value("entry_price", 0.0);
            r.exit_price                  = j.value("exit_price", 0.0);
            r.size_usdc                   = j.value("size_usdc", 0.0);
            r.shares                      = j.value("shares", 0.0);
            r.btc_price_at_entry          = j.value("btc_price", 0.0);
            r.btc_strike                  = j.value("btc_strike", 0.0);
            r.btc_deviation_pct           = j.value("btc_deviation_pct", 0.0);
            r.entry_vol                   = j.value("entry_vol", 0.0);
            r.avg_vol                     = j.value("avg_vol", 0.0);
            r.exit_reason                 = j.value("exit_reason", "");
            r.realized_pnl                = j.value("pnl", 0.0);
            r.fee_paid                    = j.value("fee", 0.0);
            r.max_price                   = j.value("max_price", 0.0);
            r.min_price                   = j.value("min_price", 0.0);
            r.btc_price_at_exit           = j.value("btc_price_at_exit", 0.0);
            r.btc_deviation_at_exit       = j.value("btc_deviation_at_exit", 0.0);
            r.spread_at_entry             = j.value("spread_at_entry", 0.0);
            r.ask_depth_at_entry          = j.value("ask_depth_at_entry", 0.0);
            r.hour_et                     = j.value("hour_et", -1);
            r.day_of_week                 = j.value("day_of_week", -1);
            r.consec_wins_before          = j.value("consec_wins_before", 0);
            r.consec_losses_before        = j.value("consec_losses_before", 0);
            r.balance_before              = j.value("balance_before", 0.0);
            r.hold_duration_sec           = j.value("hold_duration_sec", 0);
            r.mfe_capture_rate            = j.value("mfe_capture_rate", 0.0);
            r.mfe_at_5min                 = j.value("mfe_at_5min", 0.0);
            r.mfe_at_10min                = j.value("mfe_at_10min", 0.0);
            r.mfe_at_15min                = j.value("mfe_at_15min", 0.0);
            r.mfe5_gain_pct               = j.value("mfe5_gain_pct", 0.0);
            r.mfe10_gain_pct              = j.value("mfe10_gain_pct", 0.0);
            r.mfe15_gain_pct              = j.value("mfe15_gain_pct", 0.0);
            r.dw_btc_dev_pct              = j.value("dw_btc_dev_pct", 0.0);
            r.dw_vol_ratio                = j.value("dw_vol_ratio", 0.0);
            r.dw_spread                   = j.value("dw_spread", 0.0);
            r.dw_dev_favors               = j.value("dw_dev_favors", false);
            records_.push_back(std::move(r));
            ++loaded;
        } catch (const std::exception&) {
            ++skipped;  // 损坏 / 半行
        }
    }
    spdlog::info("TradeJournal loaded {} historical record(s) from {}{}",
                 loaded, path_,
                 skipped ? (" (" + std::to_string(skipped) + " line(s) skipped)") : "");
}

void TradeJournal::record(const TradeRecord& trade) {
    std::lock_guard<std::mutex> lock(mu_);
    records_.push_back(trade);

    // 追加写入 JSONL 文件
    json j;
    j["id"] = trade.id;
    j["mode"] = trade.mode;
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
    j["mfe_at_5min"] = trade.mfe_at_5min;
    j["mfe_at_10min"] = trade.mfe_at_10min;
    j["mfe_at_15min"] = trade.mfe_at_15min;
    j["mfe5_gain_pct"] = trade.mfe5_gain_pct;
    j["mfe10_gain_pct"] = trade.mfe10_gain_pct;
    j["mfe15_gain_pct"] = trade.mfe15_gain_pct;
    // dead_water 触发时的上下文埋点（仅 dead_water_exit 时有意义；其它 exit 全为默认 0/false）
    j["dw_btc_dev_pct"]  = trade.dw_btc_dev_pct;
    j["dw_vol_ratio"]    = trade.dw_vol_ratio;
    j["dw_spread"]       = trade.dw_spread;
    j["dw_dev_favors"]   = trade.dw_dev_favors;

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
