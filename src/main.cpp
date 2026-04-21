#include <chrono>
#include <csignal>
#include <filesystem>
#include <mutex>
#include <thread>

#include <json.hpp>
#include <spdlog/spdlog.h>

#include "core/binance_feed.h"
#include "core/market_feed.h"
#include "core/risk_manager.h"
#include "core/strategy.h"
#include "core/trade_journal.h"
#include "dashboard.h"
#include "net/api_server.h"
#include "utils/config.h"

using json = nlohmann::json;

static volatile bool g_running = true;
static void signal_handler(int) { g_running = false; }

static std::string find_config(const char* argv0) {
    namespace fs = std::filesystem;
    if (fs::exists("config/config.json")) return "config/config.json";
    auto dir = fs::weakly_canonical(fs::path(argv0)).parent_path();
    for (int i = 0; i < 5; i++) {
        auto candidate = dir / "config" / "config.json";
        if (fs::exists(candidate)) return candidate.string();
        if (!dir.has_parent_path() || dir == dir.parent_path()) break;
        dir = dir.parent_path();
    }
    return "config/config.json";
}

static double calc_fee(double shares, double price) {
    return shares * 0.05 * price * (1.0 - price);
}

struct UpDownQuotes {
    double up_ask = 0, down_ask = 0;
    double up_bid = 0, down_bid = 0;
    std::string up_token_id, down_token_id;
};

static UpDownQuotes extract_quotes(const polymarket::MarketEntry& entry) {
    UpDownQuotes q;
    for (const auto& token : entry.market.tokens) {
        auto it = entry.best_prices.find(token.token_id);
        if (it == entry.best_prices.end()) continue;
        if (token.outcome == "Up" || token.outcome == "Yes") {
            q.up_ask = it->second.best_ask;
            q.up_bid = it->second.best_bid;
            q.up_token_id = token.token_id;
        } else {
            q.down_ask = it->second.best_ask;
            q.down_bid = it->second.best_bid;
            q.down_token_id = token.token_id;
        }
    }
    return q;
}

static int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

// === 共享状态（策略线程写，API 线程读） ===
struct SharedState {
    std::mutex mu;
    std::string mode;
    int tick_count = 0;
    int64_t start_time = 0;
    polymarket::BtcMarketData btc;
    std::vector<polymarket::Position> positions;
    int consecutive_losses = 0;
    double daily_pnl = 0;
};

static int next_position_id = 1;

int main(int argc, char* argv[]) {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::string config_path = (argc > 1) ? argv[1] : find_config(argv[0]);

    polymarket::AppConfig cfg;
    try {
        cfg = polymarket::load_config(config_path);
    } catch (const std::exception& e) {
        spdlog::error("Failed to load config: {}", e.what());
        return 1;
    }

    polymarket::init_logging(cfg.logging);
    spdlog::info("polymarket-arb v0.3.0 [{}]",
                 cfg.strategy.mode == "live" ? "LIVE" : "DRY RUN");

    // 初始化模块
    polymarket::BinanceFeed binance(cfg.network.proxy_url);
    polymarket::MarketFeed market_feed(cfg);
    polymarket::Strategy strategy(cfg);
    polymarket::RiskManager risk(cfg);
    polymarket::TradeJournal journal("./logs/trades.jsonl");

    // 共享状态
    SharedState state;
    state.mode = cfg.strategy.mode;
    state.start_time = now_ms();

    // 持仓跟踪
    std::vector<polymarket::Position> positions;

    // === API 服务器 ===
    polymarket::net::ApiServer api(cfg.network.api_port);
    api.set_dashboard_html(polymarket::DASHBOARD_HTML);

    api.on_status([&]() -> std::string {
        std::lock_guard<std::mutex> lock(state.mu);
        json j;
        j["mode"] = state.mode;
        j["tick_count"] = state.tick_count;
        j["btc_price"] = state.btc.current_price;
        j["btc_strike"] = state.btc.strike_price;
        j["btc_deviation_pct"] = state.btc.deviation_pct;
        j["current_vol"] = state.btc.current_1h_vol;
        j["avg_vol"] = state.btc.avg_24h_vol;
        j["minutes_remaining"] = state.btc.minutes_remaining;
        j["open_positions"] = static_cast<int>(state.positions.size());
        j["consecutive_losses"] = state.consecutive_losses;
        j["daily_pnl"] = state.daily_pnl;
        j["account_balance"] = risk.account_balance();

        // 计算持仓汇总：买入成本、未实现盈亏
        double total_cost = 0;
        double total_unrealized = 0;
        for (const auto& p : state.positions) {
            double cost = p.shares * p.entry_price;
            double current_value = p.shares * p.current_price * p.shares_remaining_pct;
            double cost_remaining = p.shares * p.entry_price * p.shares_remaining_pct;
            total_cost += cost;
            total_unrealized += (current_value - cost_remaining);
        }
        j["total_cost"] = total_cost;
        j["unrealized_pnl"] = total_unrealized;

        // uptime
        int64_t elapsed_sec = (now_ms() - state.start_time) / 1000;
        int h = static_cast<int>(elapsed_sec / 3600);
        int m = static_cast<int>((elapsed_sec % 3600) / 60);
        int s = static_cast<int>(elapsed_sec % 60);
        char buf[32];
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, m, s);
        j["uptime"] = buf;

        // positions detail
        json pos_arr = json::array();
        for (const auto& p : state.positions) {
            json pj;
            pj["id"] = p.id;
            pj["side"] = (p.side == polymarket::Side::UP) ? "UP" : "DOWN";
            pj["entry_price"] = p.entry_price;
            pj["current_price"] = p.current_price;
            pj["shares"] = p.shares;
            pj["remaining_pct"] = p.shares_remaining_pct;
            pj["entry_time"] = p.entry_time;
            pj["market"] = p.market_question;
            pos_arr.push_back(pj);
        }
        j["positions"] = pos_arr;
        return j.dump();
    });

    api.on_trades([&]() -> std::string {
        json arr = json::array();
        for (const auto& t : journal.records()) {
            json j;
            j["id"] = t.id;
            j["market"] = t.market_question;
            j["side"] = t.side;
            j["entry_time"] = t.entry_time;
            j["exit_time"] = t.exit_time;
            j["entry_price"] = t.entry_price;
            j["exit_price"] = t.exit_price;
            j["shares"] = t.shares;
            j["size_usdc"] = t.size_usdc;
            j["exit_reason"] = t.exit_reason;
            j["pnl"] = t.realized_pnl;
            j["fee"] = t.fee_paid;
            j["btc_price"] = t.btc_price_at_entry;
            j["btc_deviation"] = t.btc_deviation_pct;
            j["vol"] = t.entry_vol;
            j["minutes_remaining"] = t.minutes_remaining_at_entry;
            arr.push_back(j);
        }
        return arr.dump();
    });

    api.on_stats([&]() -> std::string {
        std::lock_guard<std::mutex> lock(state.mu);
        json j;
        j["total_trades"] = journal.candle_count();
        j["wins"] = journal.candle_wins();
        j["losses"] = journal.candle_losses();
        j["total_pnl"] = journal.total_pnl();
        j["win_rate"] = journal.candle_win_rate() * 100.0;
        j["daily_pnl"] = state.daily_pnl;
        return j.dump();
    });

    api.start();

    int poll_sec = cfg.strategy.poll_interval_sec;
    spdlog::info("Strategy loop: poll every {}s, account=${:.0f}, dashboard at http://localhost:{}",
                 poll_sec, cfg.strategy.account_balance, cfg.network.api_port);

    // === 策略主循环 ===
    double last_up_ask = 0, last_down_ask = 0;
    std::string last_market_slug;  // 跟踪市场切换，检测新 K线
    while (g_running) {
        try {
            auto btc = binance.fetch();

            // 更新共享状态
            {
                std::lock_guard<std::mutex> lock(state.mu);
                state.btc = btc;
                state.tick_count++;
                state.consecutive_losses = risk.consecutive_losses();
                state.daily_pnl = risk.daily_pnl();
            }

            market_feed.fetch_markets();
            if (market_feed.market_count() == 0) {
                spdlog::info("No active BTC 1h markets, waiting...");
                std::this_thread::sleep_for(std::chrono::seconds(poll_sec));
                continue;
            }

            // 检测 K线 切换（市场 slug 变了 = 新一小时）
            for (const auto& [cid, entry] : market_feed.markets()) {
                if (last_market_slug != entry.market.market_slug) {
                    if (!last_market_slug.empty()) {
                        risk.reset_candle();
                        spdlog::info("New candle: {} → reset stop flag", entry.market.market_slug);
                    }
                    last_market_slug = entry.market.market_slug;
                }
                break;  // 只看第一个市场
            }

            // 遍历市场，刷新订单簿，评估信号
            for (const auto& [cid, entry] : market_feed.markets()) {
                if (!g_running) break;

                market_feed.refresh_order_book(cid);
                auto updated = market_feed.get_market(cid);
                if (!updated || updated->best_prices.size() < 2) continue;

                auto quotes = extract_quotes(*updated);
                if (quotes.up_ask <= 0 && quotes.down_ask <= 0) continue;

                last_up_ask = quotes.up_ask;
                last_down_ask = quotes.down_ask;

                spdlog::info("Market: {} | Up: {:.3f}/{:.3f} | Down: {:.3f}/{:.3f}",
                             entry.market.question,
                             quotes.up_bid, quotes.up_ask,
                             quotes.down_bid, quotes.down_ask);

                auto sig = strategy.evaluate_entry(
                    btc, quotes.up_ask, quotes.down_ask,
                    quotes.up_token_id, quotes.down_token_id,
                    cid, entry.market.question,
                    btc.minutes_remaining);

                if (sig.valid) {
                    std::string risk_reject;
                    if (risk.can_open_position(sig, risk_reject)) {
                        double shares = risk.compute_position_size();  // 固定 5 shares
                        double size = shares * sig.entry_price;
                        double fee = calc_fee(shares, sig.entry_price);

                        polymarket::Position pos;
                        pos.id = "P" + std::to_string(next_position_id++);
                        pos.side = sig.side;
                        pos.token_id = sig.token_id;
                        pos.condition_id = sig.condition_id;
                        pos.market_question = sig.market_question;
                        pos.entry_price = sig.entry_price;
                        pos.current_price = sig.market_ask;
                        pos.size_usdc = size;
                        pos.shares = shares;
                        pos.btc_price_at_entry = btc.current_price;
                        pos.btc_strike_at_entry = btc.strike_price;
                        pos.entry_vol = btc.current_1h_vol;
                        pos.avg_vol = btc.avg_24h_vol;
                        pos.entry_fee = fee;
                        pos.entry_time = now_ms();
                        pos.minutes_remaining_at_entry = btc.minutes_remaining;
                        pos.tp_levels = strategy.compute_tp_levels(sig.entry_price);

                        positions.push_back(pos);
                        risk.add_position();
                        risk.deduct_balance(size + fee);  // 动态余额：扣除成本+买入手续费

                        spdlog::info("OPEN [{}] {} {} @ {:.3f} | ${:.2f} ({} shares) | fee=${:.2f} | balance=${:.2f}",
                                     cfg.strategy.mode, pos.id,
                                     (pos.side == polymarket::Side::UP ? "UP" : "DOWN"),
                                     pos.entry_price, size, shares, fee, risk.account_balance());
                    } else {
                        spdlog::info("Signal blocked by risk: {}", risk_reject);
                    }
                } else if (!sig.reject_reason.empty()) {
                    spdlog::debug("No signal: {}", sig.reject_reason);
                }
            }

            // 管理现有持仓
            for (auto& pos : positions) {
                if (pos.closed || !g_running) continue;

                auto updated = market_feed.get_market(pos.condition_id);
                if (!updated) {
                    // 市场已到期/被清理，强制平仓（模拟到期结算）
                    double last_price = pos.current_price;
                    double remaining_shares = pos.shares * pos.shares_remaining_pct;
                    double sell_value = remaining_shares * last_price;
                    double cost_basis = remaining_shares * pos.entry_price;
                    double exit_fee = calc_fee(remaining_shares, last_price);
                    double entry_fee_portion = calc_fee(remaining_shares, pos.entry_price);
                    double pnl = sell_value - cost_basis - exit_fee - entry_fee_portion;

                    pos.realized_pnl += pnl;
                    pos.closed = true;
                    pos.close_reason = "expired";
                    risk.remove_position();
                    risk.add_balance(sell_value - exit_fee);  // 动态余额：回收卖出收入

                    if (pnl > 0) risk.record_profit(pnl);
                    else risk.record_loss(-pnl);

                    spdlog::warn("EXPIRED [{}] {} market gone, settle @ last_price={:.3f} | pnl=${:+.2f} | balance=${:.2f}",
                                 pos.id,
                                 (pos.side == polymarket::Side::UP ? "UP" : "DOWN"),
                                 last_price, pnl, risk.account_balance());

                    polymarket::TradeRecord rec;
                    rec.id = pos.id;
                    rec.market_question = pos.market_question;
                    rec.side = (pos.side == polymarket::Side::UP) ? "UP" : "DOWN";
                    rec.entry_time = pos.entry_time;
                    rec.exit_time = now_ms();
                    rec.minutes_remaining_at_entry = pos.minutes_remaining_at_entry;
                    rec.entry_price = pos.entry_price;
                    rec.exit_price = last_price;
                    rec.size_usdc = remaining_shares * pos.entry_price;
                    rec.shares = remaining_shares;
                    rec.btc_price_at_entry = pos.btc_price_at_entry;
                    rec.btc_strike = pos.btc_strike_at_entry;
                    rec.btc_deviation_pct = (pos.btc_price_at_entry - pos.btc_strike_at_entry) /
                                             pos.btc_strike_at_entry * 100.0;
                    rec.entry_vol = pos.entry_vol;
                    rec.avg_vol = pos.avg_vol;
                    rec.exit_reason = "expired";
                    rec.realized_pnl = pnl;  // 只记本次卖出的 P&L，不是累积
                    rec.fee_paid = exit_fee + entry_fee_portion;
                    journal.record(rec);
                    continue;
                }

                double current_price = 0;
                for (const auto& token : updated->market.tokens) {
                    if (token.token_id == pos.token_id) {
                        auto it = updated->best_prices.find(token.token_id);
                        if (it != updated->best_prices.end()) {
                            current_price = it->second.best_bid;
                        }
                        break;
                    }
                }
                if (current_price <= 0) continue;
                pos.current_price = current_price;

                // 检查止盈
                for (auto& tp : pos.tp_levels) {
                    if (tp.triggered) continue;
                    if (current_price >= tp.trigger_price) {
                        tp.triggered = true;
                        double sell_shares = pos.shares * tp.sell_pct * pos.shares_remaining_pct;
                        double sell_value = sell_shares * current_price;
                        double cost_basis = sell_shares * pos.entry_price;
                        double exit_fee = calc_fee(sell_shares, current_price);
                        double entry_fee_portion = calc_fee(sell_shares, pos.entry_price);
                        double pnl = sell_value - cost_basis - exit_fee - entry_fee_portion;

                        pos.shares_remaining_pct -= tp.sell_pct * pos.shares_remaining_pct;
                        pos.realized_pnl += pnl;
                        risk.add_balance(sell_value - exit_fee);  // 动态余额：回收卖出收入

                        spdlog::info("TP{} [{}]: sell {:.1f} shares @ {:.3f} | pnl=${:+.4f} | remaining={:.0f}% | balance=${:.2f}",
                                     tp.tier, pos.id, sell_shares, current_price,
                                     pnl, pos.shares_remaining_pct * 100, risk.account_balance());

                        if (pnl > 0) risk.record_profit(pnl);
                        else risk.record_loss(-pnl);

                        // 记录每次 TP 卖出到日志
                        polymarket::TradeRecord tp_rec;
                        tp_rec.id = pos.id + "-TP" + std::to_string(tp.tier);
                        tp_rec.market_question = pos.market_question;
                        tp_rec.side = (pos.side == polymarket::Side::UP) ? "UP" : "DOWN";
                        tp_rec.entry_time = pos.entry_time;
                        tp_rec.exit_time = now_ms();
                        tp_rec.minutes_remaining_at_entry = pos.minutes_remaining_at_entry;
                        tp_rec.entry_price = pos.entry_price;
                        tp_rec.exit_price = current_price;
                        tp_rec.size_usdc = sell_shares * pos.entry_price;
                        tp_rec.shares = sell_shares;
                        tp_rec.btc_price_at_entry = pos.btc_price_at_entry;
                        tp_rec.btc_strike = pos.btc_strike_at_entry;
                        tp_rec.btc_deviation_pct = (pos.btc_price_at_entry - pos.btc_strike_at_entry) /
                                                     pos.btc_strike_at_entry * 100.0;
                        tp_rec.entry_vol = pos.entry_vol;
                        tp_rec.avg_vol = pos.avg_vol;
                        tp_rec.exit_reason = "tp" + std::to_string(tp.tier);
                        tp_rec.realized_pnl = pnl;
                        tp_rec.fee_paid = exit_fee + entry_fee_portion;
                        journal.record(tp_rec);
                    }
                }

                // TP 卖光后自���关闭仓位（TP 子记录已完整记录所有卖出，无需额外 journal 记录）
                if (pos.shares_remaining_pct < 0.01 && !pos.closed) {
                    pos.closed = true;
                    pos.close_reason = "tp_filled";
                    risk.remove_position();

                    spdlog::info("ALL TP FILLED [{}]: total_rpnl=${:+.2f} | balance=${:.2f}",
                                 pos.id, pos.realized_pnl, risk.account_balance());
                    continue;
                }

                // 检查止损
                auto exit_sig = strategy.evaluate_exit(pos, current_price, btc, btc.minutes_remaining);
                if (exit_sig.should_exit) {
                    double remaining_shares = pos.shares * pos.shares_remaining_pct;
                    double sell_value = remaining_shares * exit_sig.exit_price;
                    double cost_basis = remaining_shares * pos.entry_price;
                    double exit_fee = calc_fee(remaining_shares, exit_sig.exit_price);
                    double entry_fee_portion = calc_fee(remaining_shares, pos.entry_price);
                    double pnl = sell_value - cost_basis - exit_fee - entry_fee_portion;

                    pos.realized_pnl += pnl;
                    pos.closed = true;
                    pos.close_reason = exit_sig.reason;
                    risk.remove_position();
                    risk.add_balance(sell_value - exit_fee);  // 动态余额：回收卖出收入

                    // §三 止损后本场不再交易
                    if (exit_sig.reason == "stop_price" || exit_sig.reason == "stop_time") {
                        risk.set_candle_stopped();
                        spdlog::warn("Candle stopped: {} triggered, no more trades this candle",
                                     exit_sig.reason);
                    }

                    if (pnl > 0) risk.record_profit(pnl);
                    else risk.record_loss(-pnl);

                    spdlog::info("CLOSE [{}] {} reason={} | pnl=${:+.2f} | balance=${:.2f}",
                                 pos.id,
                                 (pos.side == polymarket::Side::UP ? "UP" : "DOWN"),
                                 exit_sig.reason, pnl, risk.account_balance());

                    polymarket::TradeRecord rec;
                    rec.id = pos.id;
                    rec.market_question = pos.market_question;
                    rec.side = (pos.side == polymarket::Side::UP) ? "UP" : "DOWN";
                    rec.entry_time = pos.entry_time;
                    rec.exit_time = now_ms();
                    rec.minutes_remaining_at_entry = pos.minutes_remaining_at_entry;
                    rec.entry_price = pos.entry_price;
                    rec.exit_price = exit_sig.exit_price;
                    rec.size_usdc = remaining_shares * pos.entry_price;
                    rec.shares = remaining_shares;
                    rec.btc_price_at_entry = pos.btc_price_at_entry;
                    rec.btc_strike = pos.btc_strike_at_entry;
                    rec.btc_deviation_pct = (pos.btc_price_at_entry - pos.btc_strike_at_entry) /
                                             pos.btc_strike_at_entry * 100.0;
                    rec.entry_vol = pos.entry_vol;
                    rec.avg_vol = pos.avg_vol;
                    rec.exit_reason = exit_sig.reason;
                    rec.realized_pnl = pnl;  // 只记本次卖出的 P&L，不是累积
                    rec.fee_paid = exit_fee + entry_fee_portion;
                    journal.record(rec);
                }
            }

            // 清理已关闭持仓
            positions.erase(
                std::remove_if(positions.begin(), positions.end(),
                               [](const polymarket::Position& p) { return p.closed; }),
                positions.end());

            // 同步持仓到共享状态
            {
                std::lock_guard<std::mutex> lock(state.mu);
                state.positions = positions;
            }

        } catch (const std::exception& e) {
            spdlog::error("Strategy loop error: {}", e.what());
        }

        spdlog::info("--- tick #{}, {} open positions, Up ask={:.3f}, Down ask={:.3f}, waiting {}s ---",
                     state.tick_count, positions.size(), last_up_ask, last_down_ask, poll_sec);
        for (int i = 0; i < poll_sec && g_running; i++) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    spdlog::info("Shutting down...");
    api.stop();
    journal.print_summary();
    spdlog::info("Done.");
    return 0;
}
