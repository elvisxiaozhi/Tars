#include <chrono>
#include <csignal>
#include <ctime>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>

#include <json.hpp>
#include <spdlog/spdlog.h>

#include "core/binance_feed.h"
#include "core/market_feed.h"
#include "core/risk_manager.h"
#include "core/live_trader.h"
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

// Polymarket 真实费率模型：maker 0%；Crypto taker 7.2% × p × (1-p)；额外 gas/tx
static double calc_fee(double shares, double price, bool is_taker,
                       const polymarket::FeeConfig& fees) {
    double rate = is_taker ? fees.taker_fee_rate : fees.maker_fee_rate;
    return shares * rate * price * (1.0 - price) + fees.gas_per_tx_usdc;
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

// 判断给定 UTC 日期是否处于美国夏令时（3月第2周日 02:00 EST ~ 11月第1周日 02:00 EDT）
static bool is_us_dst(int year, int month, int day, int utc_hour) {
    if (month < 3 || month > 11) return false;
    if (month > 3 && month < 11) return true;
    // 计算给定月份第 1 天是星期几
    std::tm t = {};
    t.tm_year = year - 1900;
    t.tm_mon = month - 1;
    t.tm_mday = 1;
    t.tm_hour = 12;
    std::mktime(&t);
    int first_sunday = 1 + (7 - t.tm_wday) % 7;
    if (month == 3) {
        int dst_start = first_sunday + 7;  // 第2个周日
        if (day > dst_start) return true;
        if (day < dst_start) return false;
        return utc_hour >= 7;  // 02:00 EST = 07:00 UTC
    } else {  // November
        int dst_end = first_sunday;  // 第1个周日
        if (day < dst_end) return true;
        if (day > dst_end) return false;
        return utc_hour < 6;  // 02:00 EDT = 06:00 UTC
    }
}

// 获取当前 ET 小时和星期（自动判断 DST：EDT=UTC-4 / EST=UTC-5）
static void get_et_time(int& hour_et, int& day_of_week) {
    auto now_t = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now_t);
    std::tm tm_utc;
    gmtime_r(&tt, &tm_utc);
    bool dst = is_us_dst(tm_utc.tm_year + 1900, tm_utc.tm_mon + 1,
                         tm_utc.tm_mday, tm_utc.tm_hour);
    int offset = dst ? 4 : 5;
    int total_hours = tm_utc.tm_hour - offset;
    if (total_hours < 0) {
        total_hours += 24;
        day_of_week = (tm_utc.tm_wday + 6) % 7;  // 跨天：星期减一
    } else {
        day_of_week = tm_utc.tm_wday;
    }
    hour_et = total_hours;
}

// 将 analytics 字段从 Position 填充到 TradeRecord
static void fill_analytics(polymarket::TradeRecord& rec,
                           const polymarket::Position& pos,
                           const polymarket::BtcMarketData& btc) {
    rec.max_price = pos.max_price;
    rec.min_price = (pos.min_price > 1e8) ? pos.entry_price : pos.min_price;
    rec.btc_price_at_exit = btc.current_price;
    rec.btc_deviation_at_exit = (btc.current_price - pos.btc_strike_at_entry)
                                 / pos.btc_strike_at_entry * 100.0;
    rec.spread_at_entry = pos.spread_at_entry;
    rec.ask_depth_at_entry = pos.ask_depth_at_entry;
    rec.hour_et = pos.hour_et;
    rec.day_of_week = pos.day_of_week;
    rec.consec_wins_before = pos.consec_wins_before;
    rec.consec_losses_before = pos.consec_losses_before;
    rec.balance_before = pos.balance_before;
    rec.hold_duration_sec = static_cast<int>((now_ms() - pos.entry_time) / 1000);
    double mfe_range = pos.max_price - pos.entry_price;
    rec.mfe_capture_rate = (mfe_range > 0.001)
        ? (rec.exit_price - pos.entry_price) / mfe_range : 0;
    rec.mfe_at_5min = pos.mfe_at_5min;
    rec.mfe_at_10min = pos.mfe_at_10min;
    rec.mfe_at_15min = pos.mfe_at_15min;
    if (pos.entry_price > 0) {
        rec.mfe5_gain_pct  = (pos.mfe_at_5min  - pos.entry_price) / pos.entry_price;
        rec.mfe10_gain_pct = (pos.mfe_at_10min - pos.entry_price) / pos.entry_price;
        rec.mfe15_gain_pct = (pos.mfe_at_15min - pos.entry_price) / pos.entry_price;
    }
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

    // === Live 模式启动守卫（渐进式放宽） ===
    // 每完成一阶段（R2/R3/.../R9）守卫会通过对应自检；之后未实现的阶段仍然 fail-fast。
    // R4 进度：EIP-712 密码库就绪（纯库，无守卫步骤）；R5-R9 仍未实现。
    if (cfg.strategy.mode == "live") {
        polymarket::LiveTrader trader(cfg);

        // R2: 钱包加载（解密 keystore.enc + 派生地址 + 与 cfg.wallet.address 比对）
        try {
            trader.init_wallet();
        } catch (const std::exception& e) {
            spdlog::error("LIVE MODE: wallet init failed: {}", e.what());
            return 1;
        }

        // R3: 链上读（USDC 余额 + allowance）
        try {
            trader.read_chain_state();
        } catch (const std::exception& e) {
            spdlog::error("LIVE MODE: chain read failed: {}", e.what());
            return 1;
        }

        // R5: CLOB API key 鉴权
        try {
            trader.ensure_clob_authenticated();
        } catch (const std::exception& e) {
            spdlog::error("LIVE MODE: CLOB auth failed: {}", e.what());
            spdlog::error("提示：若 HTTP 401，检查 EIP-712 type string（ClobAuth address 字段类型）");
            return 1;
        }

        // R-V2.3: Polymarket cash balance（V2 vault ledger）
        // V2 升级后用户资金不在链上 proxy 钱包里，必须经 /balance-allowance 读 pUSD 余额
        try {
            trader.read_polymarket_balance();
        } catch (const std::exception& e) {
            spdlog::error("LIVE MODE: polymarket balance read failed: {}", e.what());
            return 1;
        }

        // R6 (V1) 已废：V2 vault 模式没有 approve CTFExchange 的概念；
        // R-V2.3 已读到 cash 余额；下一步 R-V2.5 改为 cash >= max_single_trade 的检查。
        spdlog::warn("R6 (V1 approval check) skipped — V2 vault 模式无此概念，待 R-V2.5 替换");

        // R9-V2: 启动对账 — 列出未平仓订单
        try {
            auto open_orders = trader.reconcile_on_startup();
            (void)open_orders;
        } catch (const std::exception& e) {
            spdlog::error("LIVE MODE: reconcile failed: {}", e.what());
            return 1;
        }

        // 可选 dry-test：用 --dry-test-order CLI flag 显式触发；默认不跑（避免依赖
        // 过期的 hardcode token_id）。需要时手动改 token_id 后传 flag 触发。
        bool run_dry_test = false;
        for (int i = 1; i < argc; ++i) {
            if (std::string(argv[i]) == "--dry-test-order") run_dry_test = true;
        }
        if (run_dry_test) {
            spdlog::warn("--dry-test-order: 真发 1 笔 limit BUY + cancel + 1 笔 SELL（业务层 reject）");
            try {
                polymarket::EntrySignal sig;
                sig.valid           = true;
                sig.side            = polymarket::Side::UP;
                sig.entry_price     = 0.05;
                sig.market_ask      = 0.23;
                sig.market_question = "(dry-test, manual token_id)";
                // ⚠️ 跑前需 update：去 gamma-api 找一个 active BTC up/down market 拿 YES token
                sig.token_id        =
                    "6668191069090033015760586900919029140131491490577391794647144142930147112459";
                auto result = trader.place_entry_order(sig, 20.0);
                if (result.success) {
                    spdlog::info("BUY ok order_id={}", result.order_id);
                    bool cx = trader.cancel_order(result.order_id);
                    spdlog::info("  BUY cancel: {}", cx ? "✅" : "⚠️ failed");
                } else {
                    spdlog::error("BUY failed: {}", result.error);
                }

                polymarket::Position fake_pos;
                fake_pos.token_id = sig.token_id;
                fake_pos.shares   = 20.0;
                auto er = trader.place_exit_order(fake_pos, 20.0, 0.99, false, "dry-test");
                if (er.success) {
                    spdlog::info("SELL ok order_id={}", er.order_id);
                    trader.cancel_order(er.order_id);
                } else {
                    spdlog::warn("SELL rejected (业务层): {}", er.error);
                }
            } catch (const std::exception& e) {
                spdlog::error("dry-test threw: {}", e.what());
            }
        }

        // R-V2.6 (待) — 接入策略主循环 + SIGINT handler 接 emergency_close_all
        spdlog::warn("================================================================");
        spdlog::warn("LIVE MODE 启动横幅就绪（R2-R5 + R-V2.3 + R9-V2 reconcile）");
        spdlog::warn("  EOA   {}", trader.wallet_address());
        spdlog::warn("  Proxy {}", cfg.polymarket.proxy_address);
        spdlog::warn("");
        spdlog::warn("待完成：R-V2.6 主循环接入 + 真成交闭环");
        spdlog::warn("Tip: --dry-test-order 可触发非真成交 dry test（需手动更新 token_id）");
        spdlog::warn("================================================================");
        return 1;
    }

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
            pj["max_price"] = p.max_price;
            pj["min_price"] = (p.min_price > 1e8) ? p.entry_price : p.min_price;
            pj["mfe"] = p.max_price - p.entry_price;
            pj["mae"] = p.entry_price - ((p.min_price > 1e8) ? p.entry_price : p.min_price);
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
            // Analytics
            j["max_price"] = t.max_price;
            j["min_price"] = t.min_price;
            j["btc_price_at_exit"] = t.btc_price_at_exit;
            j["btc_deviation_at_exit"] = t.btc_deviation_at_exit;
            j["spread_at_entry"] = t.spread_at_entry;
            j["ask_depth_at_entry"] = t.ask_depth_at_entry;
            j["hour_et"] = t.hour_et;
            j["day_of_week"] = t.day_of_week;
            j["consec_wins_before"] = t.consec_wins_before;
            j["consec_losses_before"] = t.consec_losses_before;
            j["balance_before"] = t.balance_before;
            j["hold_duration_sec"] = t.hold_duration_sec;
            j["mfe_capture_rate"] = t.mfe_capture_rate;
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

    api.on_analytics([&]() -> std::string {
        const auto& recs = journal.records();
        json j;

        if (recs.empty()) {
            j["has_data"] = false;
            return j.dump();
        }
        j["has_data"] = true;

        // MFE/MAE 统计
        double sum_mfe = 0, sum_mae = 0, max_mfe = 0;
        int mfe_count = 0;
        // 持仓时长
        int sum_dur = 0, min_dur = 999999, max_dur = 0;
        // 出场原因分布
        std::map<std::string, int> exit_reasons;
        // 小时热力图
        std::map<int, std::pair<int, double>> hour_stats;  // hour -> {count, sum_pnl}
        // 星期统计
        std::map<int, std::pair<int, double>> day_stats;
        // Spread: win vs lose
        double spread_win_sum = 0, spread_lose_sum = 0;
        int spread_win_n = 0, spread_lose_n = 0;
        // 连亏后表现
        double consec_loss_pnl_sum = 0;
        int consec_loss_n = 0;
        double consec_ok_pnl_sum = 0;
        int consec_ok_n = 0;
        // MFE 捕获率
        double capture_sum = 0, capture_win_sum = 0, capture_lose_sum = 0;
        int capture_n = 0, capture_win_n = 0, capture_lose_n = 0;
        // 利润因子
        double gross_profit = 0, gross_loss = 0;
        // TP + 出场计数
        int tp0_hits = 0, tp1_hits = 0, tp2_hits = 0, trailing_hits = 0;
        double trailing_pnl_sum = 0, stop_price_pnl_sum = 0;
        int stop_price_count = 0;
        // 蜡烛级聚合
        struct CandleAgg { std::string side; double pnl=0; double entry_price=0;
                           double btc_dev=0; int duration=0;
                           int hour_et=-1; int day_of_week=-1; };
        std::map<std::string, CandleAgg> candle_agg;
        // 资金曲线
        json equity_arr = json::array();

        for (const auto& t : recs) {
            double mfe = t.max_price - t.entry_price;
            double mae = t.entry_price - t.min_price;
            sum_mfe += mfe;
            sum_mae += mae;
            if (mfe > max_mfe) max_mfe = mfe;
            mfe_count++;

            int dur = t.hold_duration_sec;
            sum_dur += dur;
            if (dur < min_dur) min_dur = dur;
            if (dur > max_dur) max_dur = dur;

            exit_reasons[t.exit_reason]++;

            if (t.realized_pnl > 0) {
                spread_win_sum += t.spread_at_entry;
                spread_win_n++;
            } else {
                spread_lose_sum += t.spread_at_entry;
                spread_lose_n++;
            }

            if (t.consec_losses_before >= 2) {
                consec_loss_pnl_sum += t.realized_pnl;
                consec_loss_n++;
            } else {
                consec_ok_pnl_sum += t.realized_pnl;
                consec_ok_n++;
            }

            // MFE 捕获率
            double mfe_range = t.max_price - t.entry_price;
            double cap_rate = (mfe_range > 0.001) ? (t.exit_price - t.entry_price) / mfe_range : 0;
            capture_sum += cap_rate; capture_n++;
            if (t.realized_pnl > 0) { capture_win_sum += cap_rate; capture_win_n++; }
            else { capture_lose_sum += cap_rate; capture_lose_n++; }

            // 利润因子
            if (t.realized_pnl > 0) gross_profit += t.realized_pnl;
            else gross_loss += std::abs(t.realized_pnl);

            // TP / 出场类型计数
            if (t.exit_reason == "tp0") tp0_hits++;
            else if (t.exit_reason == "tp1") tp1_hits++;
            else if (t.exit_reason == "tp2") tp2_hits++;
            else if (t.exit_reason == "trailing_stop") { trailing_hits++; trailing_pnl_sum += t.realized_pnl; }
            else if (t.exit_reason == "stop_price") { stop_price_pnl_sum += t.realized_pnl; stop_price_count++; }

            // 蜡烛级聚合
            {
                std::string base_id = t.id;
                auto tp_pos = base_id.find("-TP");
                if (tp_pos != std::string::npos) base_id = base_id.substr(0, tp_pos);
                auto& ca = candle_agg[base_id];
                ca.pnl += t.realized_pnl;
                if (ca.side.empty()) { ca.side = t.side; ca.entry_price = t.entry_price; ca.btc_dev = t.btc_deviation_pct; }
                if (t.hold_duration_sec > ca.duration) ca.duration = t.hold_duration_sec;
                if (ca.hour_et < 0 && t.hour_et >= 0) { ca.hour_et = t.hour_et; }
                if (ca.day_of_week < 0 && t.day_of_week >= 0) { ca.day_of_week = t.day_of_week; }
            }

            // 资金曲线点
            json eq;
            eq["time"] = t.exit_time;
            eq["balance"] = t.balance_before + t.realized_pnl;
            equity_arr.push_back(eq);
        }

        // 按蜡烛聚合 hour/day 统计（避免 TP0+TP1+TP2 被重复计数）
        for (const auto& [id, ca] : candle_agg) {
            if (ca.hour_et >= 0) {
                hour_stats[ca.hour_et].first++;
                hour_stats[ca.hour_et].second += ca.pnl;
            }
            if (ca.day_of_week >= 0) {
                day_stats[ca.day_of_week].first++;
                day_stats[ca.day_of_week].second += ca.pnl;
            }
        }

        // MFE/MAE
        json mfe_mae;
        mfe_mae["avg_mfe"] = mfe_count > 0 ? sum_mfe / mfe_count : 0;
        mfe_mae["avg_mae"] = mfe_count > 0 ? sum_mae / mfe_count : 0;
        mfe_mae["max_mfe"] = max_mfe;
        mfe_mae["ratio"] = (sum_mae > 0) ? sum_mfe / sum_mae : 0;
        j["mfe_mae"] = mfe_mae;

        // 持仓时长
        json duration;
        duration["avg"] = mfe_count > 0 ? sum_dur / mfe_count : 0;
        duration["min"] = min_dur < 999999 ? min_dur : 0;
        duration["max"] = max_dur;
        j["duration"] = duration;

        // 出场原因
        json reasons = json::object();
        for (const auto& [reason, count] : exit_reasons) {
            reasons[reason] = count;
        }
        j["exit_reasons"] = reasons;

        // 小时热力图
        json hours = json::object();
        for (int h = 0; h < 24; h++) {
            auto it = hour_stats.find(h);
            json hj;
            hj["count"] = it != hour_stats.end() ? it->second.first : 0;
            hj["total_pnl"] = it != hour_stats.end() ? it->second.second : 0;
            hj["avg_pnl"] = (it != hour_stats.end() && it->second.first > 0)
                             ? it->second.second / it->second.first : 0;
            hours[std::to_string(h)] = hj;
        }
        j["hours"] = hours;

        // 星期统计
        const char* day_names[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
        json days = json::object();
        for (int d = 0; d < 7; d++) {
            auto it = day_stats.find(d);
            json dj;
            dj["count"] = it != day_stats.end() ? it->second.first : 0;
            dj["total_pnl"] = it != day_stats.end() ? it->second.second : 0;
            dj["avg_pnl"] = (it != day_stats.end() && it->second.first > 0)
                             ? it->second.second / it->second.first : 0;
            days[day_names[d]] = dj;
        }
        j["days"] = days;

        // Spread 分析
        json spread;
        spread["avg_spread_winners"] = spread_win_n > 0 ? spread_win_sum / spread_win_n : 0;
        spread["avg_spread_losers"] = spread_lose_n > 0 ? spread_lose_sum / spread_lose_n : 0;
        j["spread"] = spread;

        // 连亏后表现
        json streak;
        streak["after_2loss_avg_pnl"] = consec_loss_n > 0 ? consec_loss_pnl_sum / consec_loss_n : 0;
        streak["after_2loss_count"] = consec_loss_n;
        streak["normal_avg_pnl"] = consec_ok_n > 0 ? consec_ok_pnl_sum / consec_ok_n : 0;
        streak["normal_count"] = consec_ok_n;
        j["streak"] = streak;

        // 资金曲线
        j["equity_curve"] = equity_arr;

        // === 蜡烛级分析 ===
        int up_n=0, up_w=0, dn_n=0, dn_w=0;
        double up_pnl=0, dn_pnl=0;
        struct Bkt { int count=0; int wins=0; double sum_pnl=0; };
        Bkt ep_bkt[4], btc_bkt[4], dur_bkt[4];
        double sum_win_c=0, sum_loss_c=0;
        int n_win_c=0, n_loss_c=0;

        for (const auto& [id, ca] : candle_agg) {
            bool w = ca.pnl > 0;
            if (ca.side == "UP") { up_n++; if(w) up_w++; up_pnl += ca.pnl; }
            else { dn_n++; if(w) dn_w++; dn_pnl += ca.pnl; }
            if (w) { sum_win_c += ca.pnl; n_win_c++; }
            else { sum_loss_c += ca.pnl; n_loss_c++; }

            int epi = (ca.entry_price < 0.15) ? 0 : (ca.entry_price < 0.20) ? 1 : (ca.entry_price < 0.25) ? 2 : 3;
            ep_bkt[epi].count++; if(w) ep_bkt[epi].wins++; ep_bkt[epi].sum_pnl += ca.pnl;

            int bi = (ca.btc_dev < -0.3) ? 0 : (ca.btc_dev < 0) ? 1 : (ca.btc_dev < 0.3) ? 2 : 3;
            btc_bkt[bi].count++; if(w) btc_bkt[bi].wins++; btc_bkt[bi].sum_pnl += ca.pnl;

            int dm = ca.duration / 60;
            int di = (dm < 15) ? 0 : (dm < 30) ? 1 : (dm < 45) ? 2 : 3;
            dur_bkt[di].count++; if(w) dur_bkt[di].wins++; dur_bkt[di].sum_pnl += ca.pnl;
        }

        // Max drawdown
        double peak_bal = 0, max_dd = 0;
        for (const auto& eqp : equity_arr) {
            double bal = eqp["balance"].get<double>();
            if (bal > peak_bal) peak_bal = bal;
            double dd = peak_bal - bal;
            if (dd > max_dd) max_dd = dd;
        }

        int candle_total = static_cast<int>(candle_agg.size());
        double avg_win = n_win_c > 0 ? sum_win_c / n_win_c : 0;
        double avg_loss = n_loss_c > 0 ? sum_loss_c / n_loss_c : 0;

        // 方向统计
        json dir;
        dir["UP"] = {{"count",up_n},{"wins",up_w},{"win_rate",up_n>0?up_w*100.0/up_n:0},
                     {"avg_pnl",up_n>0?up_pnl/up_n:0},{"total_pnl",up_pnl}};
        dir["DOWN"] = {{"count",dn_n},{"wins",dn_w},{"win_rate",dn_n>0?dn_w*100.0/dn_n:0},
                       {"avg_pnl",dn_n>0?dn_pnl/dn_n:0},{"total_pnl",dn_pnl}};
        j["direction"] = dir;

        // MFE 捕获率
        json mfe_cap;
        mfe_cap["avg"] = capture_n > 0 ? capture_sum / capture_n : 0;
        mfe_cap["avg_winners"] = capture_win_n > 0 ? capture_win_sum / capture_win_n : 0;
        mfe_cap["avg_losers"] = capture_lose_n > 0 ? capture_lose_sum / capture_lose_n : 0;
        j["mfe_capture"] = mfe_cap;

        // TP 触发率（基于蜡烛数）
        json tp;
        tp["tp0"] = {{"count",tp0_hits},{"pct",candle_total>0?tp0_hits*100.0/candle_total:0}};
        tp["tp1"] = {{"count",tp1_hits},{"pct",candle_total>0?tp1_hits*100.0/candle_total:0}};
        tp["tp2"] = {{"count",tp2_hits},{"pct",candle_total>0?tp2_hits*100.0/candle_total:0}};
        tp["trailing_stop"] = {{"count",trailing_hits},{"pct",candle_total>0?trailing_hits*100.0/candle_total:0}};
        j["tp_hit_rates"] = tp;

        // 核心指标
        j["profit_factor"] = gross_loss > 0 ? gross_profit / gross_loss : 0;
        j["max_drawdown"] = max_dd;
        j["max_drawdown_pct"] = peak_bal > 0 ? max_dd / peak_bal * 100 : 0;
        j["ev_per_trade"] = candle_total > 0 ? journal.total_pnl() / candle_total : 0;
        j["avg_win"] = avg_win;
        j["avg_loss"] = avg_loss;
        j["win_loss_ratio"] = avg_loss != 0 ? std::abs(avg_win / avg_loss) : 0;

        // Trailing stop 效果
        json ts;
        ts["count"] = trailing_hits;
        ts["avg_pnl"] = trailing_hits > 0 ? trailing_pnl_sum / trailing_hits : 0;
        ts["stop_price_avg_pnl"] = stop_price_count > 0 ? stop_price_pnl_sum / stop_price_count : 0;
        j["trailing_stop_stats"] = ts;

        // 入场价分桶
        const char* ep_labels[] = {"0-15c","15-20c","20-25c","25-30c"};
        json ep_j = json::object();
        for (int i = 0; i < 4; i++) {
            ep_j[ep_labels[i]] = {{"count",ep_bkt[i].count},{"wins",ep_bkt[i].wins},
                {"win_rate",ep_bkt[i].count>0?ep_bkt[i].wins*100.0/ep_bkt[i].count:0},
                {"avg_pnl",ep_bkt[i].count>0?ep_bkt[i].sum_pnl/ep_bkt[i].count:0}};
        }
        j["entry_price_buckets"] = ep_j;

        // BTC 偏移分桶
        const char* btc_labels[] = {"<-0.3%","-0.3~0%","0~+0.3%",">+0.3%"};
        json btc_j = json::object();
        for (int i = 0; i < 4; i++) {
            btc_j[btc_labels[i]] = {{"count",btc_bkt[i].count},{"wins",btc_bkt[i].wins},
                {"win_rate",btc_bkt[i].count>0?btc_bkt[i].wins*100.0/btc_bkt[i].count:0},
                {"avg_pnl",btc_bkt[i].count>0?btc_bkt[i].sum_pnl/btc_bkt[i].count:0}};
        }
        j["btc_deviation_buckets"] = btc_j;

        // 持仓时长分桶
        const char* dur_labels[] = {"0-15m","15-30m","30-45m","45-60m"};
        json dur_j = json::object();
        for (int i = 0; i < 4; i++) {
            dur_j[dur_labels[i]] = {{"count",dur_bkt[i].count},{"wins",dur_bkt[i].wins},
                {"win_rate",dur_bkt[i].count>0?dur_bkt[i].wins*100.0/dur_bkt[i].count:0},
                {"avg_pnl",dur_bkt[i].count>0?dur_bkt[i].sum_pnl/dur_bkt[i].count:0}};
        }
        j["duration_buckets"] = dur_j;

        return j.dump();
    });

    api.start();

    int poll_sec = cfg.strategy.poll_interval_sec;
    spdlog::info("Strategy loop: poll every {}s, account=${:.0f}, dashboard at http://localhost:{}",
                 poll_sec, cfg.strategy.account_balance, cfg.network.api_port);

    // 拒绝原因聚合器：避免每个 tick 都打 reject 日志，每 10 次或 5 分钟汇总输出一行
    struct RejectAggregator {
        std::map<std::string, int> counts;
        double dev_sum = 0;
        int dev_n = 0;
        int64_t last_flush_ms = 0;
        int total() const {
            int t = 0; for (auto& [k, v] : counts) t += v; return t;
        }
        void add(const std::string& reason, double dev_pct) {
            std::string key = reason.substr(0, reason.find(':'));
            counts[key]++;
            if (key.rfind("btc_dev", 0) == 0) { dev_sum += dev_pct; dev_n++; }
        }
        bool should_flush(int64_t now_ms_) const {
            if (counts.empty()) return false;
            return total() >= 10 || (now_ms_ - last_flush_ms) >= 300000;
        }
        void reset(int64_t now_ms_) {
            counts.clear(); dev_sum = 0; dev_n = 0; last_flush_ms = now_ms_;
        }
    } reject_agg;
    reject_agg.last_flush_ms = now_ms();

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
                spdlog::info("#{} | No active markets, waiting 30s...", state.tick_count);
                for (int i = 0; i < 30 && g_running; i++)
                    std::this_thread::sleep_for(std::chrono::seconds(1));
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

                spdlog::debug("Market: {} | Up: {:.3f}/{:.3f} | Down: {:.3f}/{:.3f}",
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
                        // 入场是限价 ask-1¢ 挂单，maker 角色，免 trading fee
                        double fee = calc_fee(shares, sig.entry_price, /*is_taker=*/false, cfg.fees);

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

                        // Analytics: MFE/MAE 初始化
                        pos.max_price = sig.entry_price;
                        pos.min_price = sig.entry_price;
                        pos.mfe_at_5min = sig.entry_price;
                        pos.mfe_at_10min = sig.entry_price;
                        pos.mfe_at_15min = sig.entry_price;

                        // Analytics: 入场 spread + ask 深度
                        if (sig.side == polymarket::Side::UP) {
                            pos.spread_at_entry = quotes.up_ask - quotes.up_bid;
                        } else {
                            pos.spread_at_entry = quotes.down_ask - quotes.down_bid;
                        }
                        // ask 侧总挂单量
                        auto ob_it = updated->order_books.find(sig.token_id);
                        if (ob_it != updated->order_books.end()) {
                            for (const auto& lvl : ob_it->second.asks)
                                pos.ask_depth_at_entry += lvl.size;
                        }

                        // Analytics: 时间标签
                        get_et_time(pos.hour_et, pos.day_of_week);

                        // Analytics: 风控上下文
                        pos.consec_wins_before = risk.consecutive_wins();
                        pos.consec_losses_before = risk.consecutive_losses();
                        pos.balance_before = risk.account_balance();

                        positions.push_back(pos);
                        risk.add_position();
                        risk.deduct_balance(size + fee);  // 动态余额：扣除成本+买入手续费

                        spdlog::info("OPEN [{}] {} {} @ {:.3f} | ${:.2f} ({} shares) | fee=${:.2f} | balance=${:.2f}",
                                     cfg.strategy.mode, pos.id,
                                     (pos.side == polymarket::Side::UP ? "UP" : "DOWN"),
                                     pos.entry_price, size, shares, fee, risk.account_balance());
                    } else {
                        spdlog::debug("Signal blocked: {}", risk_reject);
                    }
                } else if (!sig.reject_reason.empty()) {
                    reject_agg.add(sig.reject_reason, btc.deviation_pct);
                }
            }

            // 管理现有持仓
            for (auto& pos : positions) {
                if (pos.closed || !g_running) continue;

                auto updated = market_feed.get_market(pos.condition_id);
                if (!updated) {
                    // 市场已到期/被清理，强制平仓（模拟到期结算）：到期自动结算，无 taker 动作
                    double last_price = pos.current_price;
                    double remaining_shares = pos.shares * pos.shares_remaining_pct;
                    double sell_value = remaining_shares * last_price;
                    double cost_basis = remaining_shares * pos.entry_price;
                    double exit_fee = calc_fee(remaining_shares, last_price, /*is_taker=*/false, cfg.fees);
                    double entry_fee_portion = calc_fee(remaining_shares, pos.entry_price, /*is_taker=*/false, cfg.fees);
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
                    fill_analytics(rec, pos, btc);
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

                // MFE/MAE 追踪
                if (current_price > pos.max_price) pos.max_price = current_price;
                if (current_price < pos.min_price) pos.min_price = current_price;

                // 时间窗口快照：入场后 ≤N 分钟时持续刷新，超过则冻结
                int64_t elapsed_sec = (now_ms() - pos.entry_time) / 1000;
                if (elapsed_sec <= 300 && current_price > pos.mfe_at_5min)
                    pos.mfe_at_5min = current_price;
                if (elapsed_sec <= 600 && current_price > pos.mfe_at_10min)
                    pos.mfe_at_10min = current_price;
                if (elapsed_sec <= 900 && current_price > pos.mfe_at_15min)
                    pos.mfe_at_15min = current_price;

                // 检查止盈
                for (auto& tp : pos.tp_levels) {
                    if (tp.triggered) continue;
                    if (current_price >= tp.trigger_price) {
                        tp.triggered = true;
                        double sell_shares = pos.shares * tp.sell_pct * pos.shares_remaining_pct;
                        double sell_value = sell_shares * current_price;
                        double cost_basis = sell_shares * pos.entry_price;
                        // TP 卖单在 best_bid 价吃单，taker 角色；入场是 maker，按 0 费摊销
                        double exit_fee = calc_fee(sell_shares, current_price, /*is_taker=*/true, cfg.fees);
                        double entry_fee_portion = calc_fee(sell_shares, pos.entry_price, /*is_taker=*/false, cfg.fees);
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
                        fill_analytics(tp_rec, pos, btc);
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
                    // 止损/拖尾/时间止损都是吃 best_bid，taker；入场 maker
                    double exit_fee = calc_fee(remaining_shares, exit_sig.exit_price, /*is_taker=*/true, cfg.fees);
                    double entry_fee_portion = calc_fee(remaining_shares, pos.entry_price, /*is_taker=*/false, cfg.fees);
                    double pnl = sell_value - cost_basis - exit_fee - entry_fee_portion;

                    pos.realized_pnl += pnl;
                    pos.closed = true;
                    pos.close_reason = exit_sig.reason;
                    risk.remove_position();
                    risk.add_balance(sell_value - exit_fee);  // 动态余额：回收卖出收入

                    // §三 止损后本场不再交易（价格止损 / 时间止损 / 移动止盈回撤 / 死水早退都算止损出场）
                    if (exit_sig.reason == "stop_price" || exit_sig.reason == "stop_time" ||
                        exit_sig.reason == "trailing_stop" || exit_sig.reason == "dead_water_exit") {
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
                    fill_analytics(rec, pos, btc);
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

        // 动态轮询间隔：末10min 5s / 有仓 10s / 空闲 15s
        // 注：移除了甜区过滤，因此低 dev 也是有效信号区，不再做深空闲节流
        int sleep_sec;
        {
            std::lock_guard<std::mutex> lock(state.mu);
            int mins = state.btc.minutes_remaining;
            if (mins <= 10) {
                sleep_sec = 5;
            } else if (!positions.empty()) {
                sleep_sec = 10;
            } else {
                sleep_sec = 15;
            }

            // 拒绝聚合器周期性 flush
            int64_t now = now_ms();
            if (reject_agg.should_flush(now)) {
                std::string s;
                for (auto& [k, v] : reject_agg.counts) {
                    if (!s.empty()) s += " ";
                    s += k + "×" + std::to_string(v);
                }
                if (reject_agg.dev_n > 0) {
                    spdlog::info("rejects ×{}: {} (avg dev={:+.3f}%)",
                                 reject_agg.total(), s, reject_agg.dev_sum / reject_agg.dev_n);
                } else {
                    spdlog::info("rejects ×{}: {}", reject_agg.total(), s);
                }
                reject_agg.reset(now);
            }

            // 紧凑状态行
            if (positions.empty()) {
                spdlog::info("#{} | {}min | BTC ${:.0f} {:+.2f}% | Up {:.2f} Dn {:.2f} | idle | ${:.2f} | {}s",
                             state.tick_count, mins,
                             state.btc.current_price, state.btc.deviation_pct,
                             last_up_ask, last_down_ask,
                             risk.account_balance(), sleep_sec);
            } else {
                for (const auto& p : positions) {
                    double chg_pct = p.entry_price > 0 ? (p.current_price - p.entry_price) / p.entry_price * 100 : 0;
                    spdlog::info("#{} | {}min | BTC ${:.0f} {:+.2f}% | {} {} {:.2f}->{:.2f} {:+.0f}% MFE:{:.2f} rem:{:.0f}% | ${:.2f} | {}s",
                                 state.tick_count, mins,
                                 state.btc.current_price, state.btc.deviation_pct,
                                 p.id, (p.side == polymarket::Side::UP ? "UP" : "DN"),
                                 p.entry_price, p.current_price, chg_pct,
                                 p.max_price, p.shares_remaining_pct * 100,
                                 risk.account_balance(), sleep_sec);
                }
            }
        }

        for (int i = 0; i < sleep_sec && g_running; i++) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    spdlog::info("Shutting down...");
    api.stop();
    journal.print_summary();
    spdlog::info("Done.");
    return 0;
}
