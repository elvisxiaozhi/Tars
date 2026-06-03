#include <chrono>
#include <csignal>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <json.hpp>
#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include "core/binance_feed.h"
#include "core/clob_ws_feed.h"
#include "core/experiment_engine.h"
#include "core/finance_feed.h"
#include "core/market_feed.h"
#include "core/quote_cache.h"
#include "core/risk_manager.h"
#include "core/live_trader.h"
#include "core/build_info.h"
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

// Polymarket 真实费率模型：maker 0%；taker = rate × p × (1-p)（Crypto 7%，见 FeeConfig）；额外 gas/tx
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

static const char* regime_name(polymarket::StrategyRegime regime);

// 主策略候选落盘：每个被策略 gate 拒掉的入场候选写一行到 logs/main_candidates.jsonl。
// 用途：量化"哪个 gate 拦掉了多少本可入场的候选/是否过紧"——实验有 *_candidates.jsonl
// 可做此分析，主策略此前没有。纯诊断旁路，不影响任何交易决策（见 step 报告 C）。
static void log_main_candidate(const std::string& coin,
                               const polymarket::BtcMarketData& md,
                               const polymarket::EntrySignal& sig,
                               const UpDownQuotes& q,
                               const std::map<std::string, polymarket::BtcMarketData>& market_ctx) {
    try {
        nlohmann::json j;
        j["ts"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch()).count();
        j["code_version"] = polymarket::code_version();
        j["config_hash"] = polymarket::config_hash();
        j["coin"] = coin;
        j["minutes_remaining"] = md.minutes_remaining;
        j["deviation_pct"] = md.deviation_pct;
        j["reject_reason"] = sig.reject_reason;
        j["regime_path"] = regime_name(sig.regime);
        j["entry_price"] = sig.entry_price;
        j["market_ask"] = sig.market_ask;
        j["up_bid"] = q.up_bid;
        j["up_ask"] = q.up_ask;
        j["down_bid"] = q.down_bid;
        j["down_ask"] = q.down_ask;
        j["up_spread"] = (q.up_ask > 0 && q.up_bid > 0) ? (q.up_ask - q.up_bid) : 0.0;
        j["down_spread"] = (q.down_ask > 0 && q.down_bid > 0) ? (q.down_ask - q.down_bid) : 0.0;
        j["entry_confidence"] = sig.entry_confidence;
        j["confidence_components"] = sig.confidence_components;
        j["entry_vol_1h"] = md.current_1h_vol;
        j["avg_vol_24h"] = md.avg_24h_vol;
        nlohmann::json cross = nlohmann::json::object();
        for (const auto& [sym, other] : market_ctx) {
            if (sym == coin) continue;
            cross[sym] = other.deviation_pct;
        }
        j["cross_coin_dev"] = cross;
        std::ofstream out("./logs/main_candidates.jsonl", std::ios::app);
        if (out) out << j.dump() << "\n";
    } catch (...) {
        // 诊断旁路，永不影响主循环
    }
}

static const char* regime_name(polymarket::StrategyRegime regime) {
    switch (regime) {
        case polymarket::StrategyRegime::LEGACY_CHEAP: return "legacy_cheap";
        case polymarket::StrategyRegime::TREND: return "trend";
        case polymarket::StrategyRegime::REVERSAL: return "reversal";
        case polymarket::StrategyRegime::QUIET_REVERSION: return "quiet_reversion";
        default: return "none";
    }
}

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
                           const polymarket::BtcMarketData& btc,
                           const std::string& mode) {
    rec.mode = mode;
    rec.coin = pos.coin;
    rec.regime = regime_name(pos.regime);
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
    rec.armed_at_ms = pos.armed_at_ms;
    rec.min_price_after_arm = pos.min_price_after_arm;
    if (pos.entry_price > 0) {
        rec.mfe5_gain_pct  = (pos.mfe_at_5min  - pos.entry_price) / pos.entry_price;
        rec.mfe10_gain_pct = (pos.mfe_at_10min - pos.entry_price) / pos.entry_price;
        rec.mfe15_gain_pct = (pos.mfe_at_15min - pos.entry_price) / pos.entry_price;
    }
    rec.tp_count_before_exit = static_cast<int>(std::count_if(
        pos.tp_levels.begin(), pos.tp_levels.end(),
        [](const polymarket::TakeProfitLevel& tp) { return tp.triggered; }));
    rec.has_tp_before_exit = rec.tp_count_before_exit > 0;
}

// === 共享状态（策略线程写，API 线程读） ===
struct SharedState {
    std::mutex mu;
    std::string mode;
    int tick_count = 0;
    int64_t start_time = 0;
    polymarket::BtcMarketData btc;
    std::map<std::string, polymarket::BtcMarketData> coins;
    std::vector<polymarket::Position> positions;
    int consecutive_losses = 0;
    double daily_pnl = 0;
    bool clob_ws_connected = false;
    int clob_ws_subscribed = 0;
    int loaded_market_count = 0;
    int entry_window_market_count = 0;
    int tradable_market_count = 0;
    int held_market_count = 0;
    // 当前活跃市场的 UP/DOWN quotes（dashboard 显示）
    double up_bid = 0, up_ask = 0;
    double down_bid = 0, down_ask = 0;
    std::string market_question;
    json markets = json::array();
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

    // 单实例 flock：阻止 systemd + manager 并发 spawn 两个 bot 都写同一份 trades.jsonl。
    // 2026-05-21 14:16 实证：双开导致同条 BTC 被两个独立 next_position_id=1 的进程各开一次，
    // 同 P-id 双 entry、共享日志与 jsonl，亏损翻倍且账本互相覆盖。fd 终生持有不显式关闭，
    // 进程退出时 OS 自动释放锁；exec 系列调用不会继承 flock（每次启动重新争抢，正确）。
    {
        namespace fs = std::filesystem;
        fs::path log_file_path(cfg.logging.file);
        fs::path log_dir = log_file_path.parent_path();
        if (log_dir.empty()) log_dir = fs::path(".");
        std::error_code ec;
        fs::create_directories(log_dir, ec);  // 失败也无妨，open 会再报错
        fs::path lock_path = log_dir / "polymarket-arb.pid";
        int lock_fd = ::open(lock_path.c_str(), O_CREAT | O_RDWR, 0644);
        if (lock_fd < 0) {
            spdlog::error("Cannot open instance lock {}: {}", lock_path.string(), std::strerror(errno));
            return 1;
        }
        if (::flock(lock_fd, LOCK_EX | LOCK_NB) < 0) {
            char buf[64] = {0};
            ssize_t n = ::read(lock_fd, buf, sizeof(buf) - 1);
            std::string holder = (n > 0) ? std::string(buf, static_cast<size_t>(n)) : std::string("?");
            spdlog::error("Another polymarket-arb instance already holds {} (pid={}). "
                          "Refusing to start to avoid duplicate orders / log corruption.",
                          lock_path.string(), holder);
            ::close(lock_fd);
            return 1;
        }
        if (::ftruncate(lock_fd, 0) != 0) {
            spdlog::warn("ftruncate({}) failed: {}", lock_path.string(), std::strerror(errno));
        }
        ::lseek(lock_fd, 0, SEEK_SET);
        std::string pid_str = std::to_string(static_cast<long>(::getpid())) + "\n";
        if (::write(lock_fd, pid_str.data(), pid_str.size()) < 0) {
            spdlog::warn("write pid to {} failed: {}", lock_path.string(), std::strerror(errno));
        }
        spdlog::info("acquired single-instance lock {} (pid={})", lock_path.string(),
                     static_cast<long>(::getpid()));
        // lock_fd 故意泄露到 main 作用域结束（进程退出时自动释放）。
        static int s_lock_fd = lock_fd;
        (void)s_lock_fd;
    }

    // LIVE 模式下持有的 trader（unique_ptr，main 作用域；mode==live 才创建）
    std::unique_ptr<polymarket::LiveTrader> live_trader;

    // === Live 模式启动守卫（渐进式放宽） ===
    // 每完成一阶段（R2/R3/.../R9）守卫会通过对应自检；之后未实现的阶段仍然 fail-fast。
    if (cfg.strategy.mode == "live") {
        live_trader = std::make_unique<polymarket::LiveTrader>(cfg);
        auto& trader = *live_trader;

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

        // R6 (V1 approval check) 已废：V2 vault 模式无 ERC-20 approve 概念；
        // cash 校验由 R-V2.3 read_polymarket_balance 已覆盖（上面那行 POLY: cash=$X.XX）。

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

        spdlog::warn("================================================================");
        spdlog::warn("LIVE MODE 启动守卫通过（R2-R5 + R-V2.3 + R9-V2）");
        spdlog::warn("  EOA   {}", trader.wallet_address());
        spdlog::warn("  Proxy {}", cfg.polymarket.proxy_address);
        spdlog::warn("  ⚠️  接入主循环：策略将真发 BUY/SELL 订单；SIGINT/SIGTERM 触发 emergency_close_all");
        spdlog::warn("================================================================");
        // 不再 return — 让主循环开始接收策略信号并真下单
    }

    // 初始化模块
    std::map<std::string, std::unique_ptr<polymarket::BinanceFeed>> binance_feeds;
    for (const auto& coin : cfg.coins) {
        binance_feeds[coin.coin] = std::make_unique<polymarket::BinanceFeed>(cfg.network.proxy_url);
    }
    polymarket::FinanceFeed finance_feed(cfg.network.proxy_url);
    polymarket::MarketFeed market_feed(cfg);
    polymarket::QuoteCache quote_cache;
    polymarket::ClobWsFeed clob_ws(cfg.polymarket.clob_ws_url,
                                   cfg.network.proxy_url, quote_cache);
    clob_ws.start();
    std::map<std::string, polymarket::Strategy> strategies;
    std::map<std::string, polymarket::CoinStrategyConfig> coin_cfg;
    for (const auto& coin : cfg.coins) {
        strategies.emplace(coin.coin, polymarket::Strategy(cfg, coin.coin));
        coin_cfg[coin.coin] = coin;
    }
    polymarket::RiskManager risk(cfg);
    polymarket::TradeJournal journal("./logs/trades.jsonl");
    // eth_cheap_v1 已于 2026-05-30 退役（全量 202 仓净 −$11.84/37% 胜率，逆势 cheap-value 失败族
    // 第 3 例，前两个亲兄弟 eth_late_cheap_v1 / QUIET_REVERSION 已退役）。
    // 详见 docs/steps/step-retire-eth-cheap-v1.md。evaluate_eth_cheap_v1 代码保留作回放，不要重新实例化。
    // eth_late_cheap_v1 已于 2026-05-23 退役。详见 docs/steps/step-retire-eth-late-cheap-v1.md。
    polymarket::ExperimentEngine finance_experiment(
        cfg, "finance_updown_v1", "./logs/experiment_finance_updown_v1_trades.jsonl", "F");
    polymarket::ExperimentEngine crypto_4h_experiment(
        cfg, "crypto_4h_updown_v1", "./logs/experiment_crypto_4h_updown_v1_trades.jsonl", "H");
    polymarket::ExperimentEngine crypto_daily_experiment(
        cfg, "crypto_daily_updown_v1", "./logs/experiment_crypto_daily_updown_v1_trades.jsonl", "D");
    polymarket::ExperimentEngine trend_v2_experiment(
        cfg, "trend_follow", "./logs/experiment_trend_v2_trades.jsonl", "T");
    // trend_v3：trend_v2 基底 + dev-accel 门解锁高价带。纯 shadow，独立持仓/jsonl。
    // 见 docs/steps/step-trend-v3-shadow.md。
    polymarket::ExperimentEngine trend_v3_experiment(
        cfg, "trend_v3", "./logs/experiment_trend_v3_trades.jsonl", "V");

    // 显示用 balance：LIVE 模式取真实 vault cash；dry_run 取虚拟 risk balance
    auto display_balance = [&]() {
        return live_trader ? live_trader->cached_cash_pusd() : risk.account_balance();
    };

    // 共享状态
    SharedState state;
    state.mode = cfg.strategy.mode;
    state.start_time = now_ms();

    // 持仓跟踪
    std::vector<polymarket::Position> positions;

    // === API 服务器 ===
    polymarket::net::ApiServer api(cfg.network.api_port, cfg.network.api_host);
    api.set_dashboard_html(polymarket::DASHBOARD_HTML);

    api.on_status([&]() -> std::string {
        std::lock_guard<std::mutex> lock(state.mu);
        json j;
        j["mode"] = state.mode;
        j["tick_count"] = state.tick_count;
        j["start_time"] = state.start_time;
        j["btc_price"] = state.btc.current_price;
        j["btc_strike"] = state.btc.strike_price;
        j["btc_deviation_pct"] = state.btc.deviation_pct;
        j["current_vol"] = state.btc.current_1h_vol;
        j["avg_vol"] = state.btc.avg_24h_vol;
        j["minutes_remaining"] = state.btc.minutes_remaining;
        j["open_positions"] = static_cast<int>(state.positions.size());
        j["consecutive_losses"] = state.consecutive_losses;
        j["daily_pnl"] = state.daily_pnl;
        j["clob_ws_connected"] = state.clob_ws_connected;
        j["clob_ws_subscribed"] = state.clob_ws_subscribed;
        j["loaded_market_count"] = state.loaded_market_count;
        j["entry_window_market_count"] = state.entry_window_market_count;
        j["tradable_market_count"] = state.tradable_market_count;
        j["held_market_count"] = state.held_market_count;
        j["account_balance"] = display_balance();
        // 当前活跃市场的 UP/DOWN quotes（dashboard metric 显示）
        j["up_bid"]   = state.up_bid;
        j["up_ask"]   = state.up_ask;
        j["down_bid"] = state.down_bid;
        j["down_ask"] = state.down_ask;
        j["market_question"] = state.market_question;
        j["markets"] = state.markets;
        json coins_j = json::array();
        for (const auto& [sym, md] : state.coins) {
            json cj;
            cj["coin"] = sym;
            cj["price"] = md.current_price;
            cj["strike"] = md.strike_price;
            cj["deviation_pct"] = md.deviation_pct;
            cj["current_vol"] = md.current_1h_vol;
            cj["avg_vol"] = md.avg_24h_vol;
            cj["minutes_remaining"] = md.minutes_remaining;
            coins_j.push_back(cj);
        }
        j["coins"] = coins_j;

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
            pj["coin"] = p.coin;
            pj["regime"] = regime_name(p.regime);
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
            j["mode"] = t.mode.empty() ? "dry_run" : t.mode;  // 老记录默认 dry_run
            j["coin"] = t.coin.empty() ? "BTC" : t.coin;
            j["regime"] = t.regime;
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
            j["has_tp_before_exit"] = t.has_tp_before_exit;
            j["tp_count_before_exit"] = t.tp_count_before_exit;
            arr.push_back(j);
        }
        return arr.dump();
    });

    api.on_stats([&]() -> std::string {
        std::lock_guard<std::mutex> lock(state.mu);
        json j;
        j["total_trades"] = journal.candle_count();
        j["wins"]         = journal.candle_wins();
        j["losses"]       = journal.candle_losses();
        j["total_pnl"]    = journal.total_pnl();
        j["win_rate"]     = journal.candle_win_rate() * 100.0;
        j["daily_pnl"]    = state.daily_pnl;

        // 按 mode 分组（按 base position id 聚合 TP partial）
        auto compute_group = [&](const std::string& target_mode) -> json {
            std::map<std::string, double> pos_pnl;
            for (const auto& r : journal.records()) {
                std::string m = r.mode.empty() ? "dry_run" : r.mode;
                if (m != target_mode) continue;
                std::string base = r.id;
                auto p = base.find("-TP");
                if (p != std::string::npos) base = base.substr(0, p);
                pos_pnl[base] += r.realized_pnl;
            }
            int wins = 0, losses = 0;
            double total = 0;
            for (auto& [_, v] : pos_pnl) {
                total += v;
                if (v > 0) ++wins; else if (v < 0) ++losses;
            }
            json g;
            g["total_trades"] = static_cast<int>(pos_pnl.size());
            g["wins"]         = wins;
            g["losses"]       = losses;
            g["total_pnl"]    = total;
            g["win_rate"]     = pos_pnl.empty() ? 0.0 : (100.0 * wins / pos_pnl.size());
            return g;
        };
        j["by_mode"]["dry_run"] = compute_group("dry_run");
        j["by_mode"]["live"]    = compute_group("live");
        return j.dump();
    });

    // 把 analytics 计算抽出来：接 records 子集 → 返回完整 analytics json。
    // 调 3 次：所有记录 / 仅 dry_run / 仅 live。
    auto compute_analytics = [](const std::vector<polymarket::TradeRecord>& recs) -> json {
        json j;
        if (recs.empty()) {
            j["has_data"] = false;
            return j;
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
        // ev_per_trade 改成基于 candle_agg 累加（避免依赖 journal.total_pnl 全局，
        // by_mode 子集才能正确计算）
        double subset_total_pnl = 0;
        for (const auto& [_, ca] : candle_agg) subset_total_pnl += ca.pnl;

        j["profit_factor"] = gross_loss > 0 ? gross_profit / gross_loss : 0;
        j["max_drawdown"] = max_dd;
        j["max_drawdown_pct"] = peak_bal > 0 ? max_dd / peak_bal * 100 : 0;
        j["ev_per_trade"] = candle_total > 0 ? subset_total_pnl / candle_total : 0;
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

        return j;
    };

    api.on_analytics([&]() -> std::string {
        // 顶层 = 所有记录
        json j = compute_analytics(journal.records());

        // by_mode 分组
        std::vector<polymarket::TradeRecord> dry, live;
        for (const auto& r : journal.records()) {
            std::string m = r.mode.empty() ? "dry_run" : r.mode;
            (m == "live" ? live : dry).push_back(r);
        }
        j["by_mode"]["dry_run"] = compute_analytics(dry);
        j["by_mode"]["live"]    = compute_analytics(live);
        return j.dump();
    });

    api.on_finance_experiment_status([&]() -> std::string {
        return finance_experiment.status_json();
    });
    api.on_finance_experiment_trades([&]() -> std::string {
        return finance_experiment.trades_json();
    });
    api.on_finance_experiment_all_trades([&]() -> std::string {
        return finance_experiment.all_trades_json();
    });
    api.on_crypto_4h_experiment_status([&]() -> std::string {
        return crypto_4h_experiment.status_json();
    });
    api.on_crypto_4h_experiment_trades([&]() -> std::string {
        return crypto_4h_experiment.trades_json();
    });
    api.on_crypto_4h_experiment_all_trades([&]() -> std::string {
        return crypto_4h_experiment.all_trades_json();
    });
    api.on_crypto_daily_experiment_status([&]() -> std::string {
        return crypto_daily_experiment.status_json();
    });
    api.on_crypto_daily_experiment_trades([&]() -> std::string {
        return crypto_daily_experiment.trades_json();
    });
    api.on_crypto_daily_experiment_all_trades([&]() -> std::string {
        return crypto_daily_experiment.all_trades_json();
    });
    api.on_trend_v2_experiment_status([&]() -> std::string {
        return trend_v2_experiment.status_json();
    });
    api.on_trend_v2_experiment_trades([&]() -> std::string {
        return trend_v2_experiment.trades_json();
    });
    api.on_trend_v2_experiment_all_trades([&]() -> std::string {
        return trend_v2_experiment.all_trades_json();
    });
    api.on_trend_v3_experiment_status([&]() -> std::string {
        return trend_v3_experiment.status_json();
    });
    api.on_trend_v3_experiment_trades([&]() -> std::string {
        return trend_v3_experiment.trades_json();
    });
    api.on_trend_v3_experiment_all_trades([&]() -> std::string {
        return trend_v3_experiment.all_trades_json();
    });

    // POST /api/shutdown — 优雅停止 bot（前端"Stop Bot"按钮触发）
    // 设 g_running=false → 主循环退出 → emergency_close_all 兜底 → 进程退出
    api.on_shutdown([&]() -> std::string {
        spdlog::warn("API: shutdown requested via /api/shutdown");
        g_running = false;
        return std::string(R"({"ok":true,"message":"Shutting down, emergency_close_all will run"})");
    });

    api.start();

    int poll_sec = cfg.strategy.poll_interval_sec;
    spdlog::info("Strategy loop: poll every {}s, account=${:.0f}, dashboard at http://{}:{}",
                 poll_sec, cfg.strategy.account_balance,
                 cfg.network.api_host, cfg.network.api_port);

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
    struct LastQuote {
        double up_bid = 0, up_ask = 0, down_bid = 0, down_ask = 0;
        std::string question;
    };
    std::map<std::string, LastQuote> last_quotes;
    std::map<std::string, int64_t> last_candle_open_by_coin;
    std::map<std::string, int64_t> binance_retry_after_ms;
    int64_t last_global_candle_open = 0;
    while (g_running) {
        try {
            // R-V2.7: LIVE 模式每 5min 刷一次真实 cash（节流，避免 rate limit）
            if (live_trader) live_trader->refresh_balance_if_stale(5 * 60 * 1000);

            std::map<std::string, polymarket::BtcMarketData> market_data;
            int64_t newest_candle_open = 0;
            struct CoinFetchResult {
                std::string coin;
                bool ok = false;
                polymarket::BtcMarketData data;
                std::string error;
            };
            std::vector<std::future<CoinFetchResult>> feed_futures;
            int64_t feed_now = now_ms();
            for (const auto& coin : cfg.coins) {
                auto retry_it = binance_retry_after_ms.find(coin.coin);
                if (retry_it != binance_retry_after_ms.end() && retry_it->second > feed_now) {
                    spdlog::debug("{} feed cooling down after previous error", coin.coin);
                    continue;
                }
                auto feed_it = binance_feeds.find(coin.coin);
                if (feed_it == binance_feeds.end()) continue;
                auto* feed = feed_it->second.get();
                feed_futures.push_back(std::async(std::launch::async, [feed, coin]() {
                    CoinFetchResult result;
                    result.coin = coin.coin;
                    try {
                        result.data = feed->fetch(coin.coin, coin.binance_symbol);
                        result.ok = true;
                    } catch (const std::exception& e) {
                        result.error = e.what();
                    }
                    return result;
                }));
            }
            for (auto& fut : feed_futures) {
                auto result = fut.get();
                if (!result.ok) {
                    spdlog::warn("{} feed error: {}", result.coin, result.error);
                    int64_t cooldown_ms = result.error.find("status=400") != std::string::npos
                        ? 5 * 60 * 1000
                        : 30 * 1000;
                    binance_retry_after_ms[result.coin] = now_ms() + cooldown_ms;
                    continue;
                }
                const auto& md = result.data;
                market_data[result.coin] = md;
                binance_retry_after_ms.erase(result.coin);
                if (md.candle_open_time > newest_candle_open) {
                    newest_candle_open = md.candle_open_time;
                }
                auto last_it = last_candle_open_by_coin.find(result.coin);
                if (last_it != last_candle_open_by_coin.end() &&
                    last_it->second != md.candle_open_time) {
                    risk.reset_candle(result.coin);
                    finance_experiment.reset_candle(result.coin);
                    crypto_4h_experiment.reset_candle(result.coin);
                    crypto_daily_experiment.reset_candle(result.coin);
                    trend_v2_experiment.reset_candle(result.coin);
                    trend_v3_experiment.reset_candle(result.coin);
                    spdlog::info("New candle [{}]: reset per-coin/global hour trade flags",
                                 result.coin);
                }
                last_candle_open_by_coin[result.coin] = md.candle_open_time;
            }
            if (market_data.empty()) {
                throw std::runtime_error("no coin market data fetched");
            }
            if (last_global_candle_open != 0 && newest_candle_open != 0 &&
                newest_candle_open != last_global_candle_open) {
                risk.reset_global_hour();
                finance_experiment.reset_global_hour();
                crypto_4h_experiment.reset_global_hour();
                crypto_daily_experiment.reset_global_hour();
                trend_v2_experiment.reset_global_hour();
                trend_v3_experiment.reset_global_hour();
                spdlog::info("New global hour: reset global trade counters");
            }
            if (newest_candle_open != 0) {
                last_global_candle_open = newest_candle_open;
            }
            auto btc_it = market_data.find("BTC");
            auto btc = btc_it != market_data.end() ? btc_it->second : market_data.begin()->second;

            // 更新共享状态
            {
                std::lock_guard<std::mutex> lock(state.mu);
                state.btc = btc;
                state.coins = market_data;
                state.tick_count++;
                state.consecutive_losses = risk.consecutive_losses();
                state.daily_pnl = risk.daily_pnl();
                state.clob_ws_connected = clob_ws.connected();
                state.clob_ws_subscribed = static_cast<int>(clob_ws.subscribed_count());
            }

            market_feed.fetch_markets(cfg.coins);
            market_feed.fetch_finance_markets();
            market_feed.fetch_crypto_duration_markets("4h");
            market_feed.fetch_crypto_duration_markets("daily");
            auto active_token_ids = market_feed.active_token_ids();
            clob_ws.update_subscriptions(active_token_ids);
            const int64_t quote_max_age_ms = live_trader ? 5000 : 10000;
            market_feed.apply_cached_quotes(quote_cache, now_ms(), quote_max_age_ms);
            if (market_feed.market_count() == 0) {
                {
                    std::lock_guard<std::mutex> lock(state.mu);
                    state.loaded_market_count = 0;
                    state.entry_window_market_count = 0;
                    state.tradable_market_count = 0;
                    state.held_market_count = 0;
                    state.markets = json::array();
                }
                spdlog::info("#{} | No active markets, waiting 30s...", state.tick_count);
                for (int i = 0; i < 30 && g_running; i++)
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }

            auto lower_text = [](std::string s) {
                for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                return s;
            };
            auto crypto_duration_for_market = [&](const polymarket::MarketEntry& entry) -> std::string {
                std::string text = lower_text(entry.market.question + " " + entry.market.market_slug);
                if (text.find("up or down") == std::string::npos) return "";
                std::string coin;
                if (text.find("bitcoin") != std::string::npos || text.find("btc") != std::string::npos) coin = "BTC";
                else if (text.find("ethereum") != std::string::npos || text.find("eth") != std::string::npos) coin = "ETH";
                else if (text.find("solana") != std::string::npos || text.find("sol") != std::string::npos) coin = "SOL";
                else if (text.find("xrp") != std::string::npos) coin = "XRP";
                else if (text.find("bnb") != std::string::npos) coin = "BNB";
                else return "";
                bool four_hour =
                    text.find("4h") != std::string::npos ||
                    text.find("4-hour") != std::string::npos ||
                    text.find("4 hour") != std::string::npos ||
                    text.find("12am-4am") != std::string::npos ||
                    text.find("4am-8am") != std::string::npos ||
                    text.find("8am-12pm") != std::string::npos ||
                    text.find("12pm-4pm") != std::string::npos ||
                    text.find("4pm-8pm") != std::string::npos ||
                    text.find("8pm-12am") != std::string::npos;
                bool hourly =
                    text.find("am et") != std::string::npos ||
                    text.find("pm et") != std::string::npos;
                if (four_hour) return "CRYPTO4H:" + coin;
                if (!hourly) return "CRYPTODAILY:" + coin;
                return "";
            };
            auto coin_for_market = [&](const polymarket::MarketEntry& entry) -> std::string {
                auto duration_coin = crypto_duration_for_market(entry);
                if (!duration_coin.empty()) return duration_coin;
                auto finance_asset = polymarket::FinanceFeed::identify_asset(entry.market);
                if (!finance_asset.empty()) return "FIN:" + finance_asset;
                for (const auto& coin : cfg.coins) {
                    if (entry.market.market_slug.rfind(coin.hourly_slug_prefix, 0) == 0) {
                        return coin.coin;
                    }
                }
                return "BTC";
            };
            {
                std::lock_guard<std::mutex> lock(state.mu);
                state.markets = json::array();
                state.loaded_market_count = static_cast<int>(market_feed.market_count());
                state.entry_window_market_count = 0;
                state.tradable_market_count = 0;
                state.held_market_count = 0;
            }

            std::set<std::string> opened_coins_this_tick;
            std::map<std::string, polymarket::BtcMarketData> finance_market_data;
            std::map<std::string, polymarket::BtcMarketData> crypto_4h_market_data;
            std::map<std::string, polymarket::BtcMarketData> crypto_daily_market_data;

            // 遍历市场，刷新订单簿，评估信号
            std::vector<const polymarket::MarketEntry*> scan_markets;
            scan_markets.reserve(market_feed.markets().size());
            for (const auto& [cid, entry] : market_feed.markets()) {
                scan_markets.push_back(&entry);
            }
            std::sort(scan_markets.begin(), scan_markets.end(),
                [&](const auto* a, const auto* b) {
                    std::string ac = coin_for_market(*a);
                    std::string bc = coin_for_market(*b);
                    if (ac == "BTC" && bc != "BTC") return true;
                    if (bc == "BTC" && ac != "BTC") return false;
                    return ac < bc;
                });

            for (const auto* market_entry : scan_markets) {
                if (!g_running) break;
                const auto& entry = *market_entry;
                const auto& cid = entry.market.condition_id;
                std::string coin = coin_for_market(entry);
                bool crypto_4h_market = coin.rfind("CRYPTO4H:", 0) == 0;
                bool crypto_daily_market = coin.rfind("CRYPTODAILY:", 0) == 0;
                if (crypto_4h_market || crypto_daily_market) {
                    std::string crypto_coin = coin.substr(crypto_4h_market ? 9 : 12);
                    auto feed_it = binance_feeds.find(crypto_coin);
                    auto cfg_it = coin_cfg.find(crypto_coin);
                    if (feed_it == binance_feeds.end() || cfg_it == coin_cfg.end()) continue;

                    auto& data_cache = crypto_4h_market ? crypto_4h_market_data : crypto_daily_market_data;
                    polymarket::BtcMarketData md;
                    auto data_it = data_cache.find(crypto_coin);
                    if (data_it == data_cache.end()) {
                        try {
                            md = crypto_4h_market
                                ? feed_it->second->fetch_period(crypto_coin, cfg_it->second.binance_symbol, "4h", 240)
                                : feed_it->second->fetch_period(crypto_coin, cfg_it->second.binance_symbol, "1d", 1440);
                            data_cache[crypto_coin] = md;
                        } catch (const std::exception& e) {
                            spdlog::debug("Crypto duration feed skipped [{}]: {}", entry.market.question, e.what());
                            continue;
                        }
                    } else {
                        md = data_it->second;
                    }

                    std::set<std::string> position_tokens;
                    for (const auto& p : positions) {
                        if (!p.closed && p.condition_id == cid) {
                            position_tokens.insert(p.token_id);
                        }
                    }

                    market_feed.apply_cached_quotes(quote_cache, now_ms(), quote_max_age_ms);
                    auto updated = market_feed.get_market(cid);
                    if (!updated) continue;
                    auto quotes = extract_quotes(*updated);
                    auto quote_now = now_ms();
                    bool up_fresh = !quotes.up_token_id.empty() &&
                        quote_cache.is_fresh(quotes.up_token_id, quote_now, quote_max_age_ms);
                    bool down_fresh = !quotes.down_token_id.empty() &&
                        quote_cache.is_fresh(quotes.down_token_id, quote_now, quote_max_age_ms);
                    bool need_rest_quote = !up_fresh || !down_fresh ||
                        quotes.up_ask <= 0 || quotes.down_ask <= 0;
                    if (need_rest_quote) {
                        bool rest_ok = !position_tokens.empty()
                            ? market_feed.refresh_order_book(cid, position_tokens)
                            : market_feed.refresh_order_book(cid);
                        if (!rest_ok) {
                            reject_agg.add((crypto_4h_market ? "crypto_4h_quote_stale:" : "crypto_daily_quote_stale:") +
                                           crypto_coin, md.deviation_pct);
                            continue;
                        }
                        updated = market_feed.get_market(cid);
                        if (!updated) continue;
                        quotes = extract_quotes(*updated);
                    }
                    if (quotes.up_ask <= 0 || quotes.down_ask <= 0) continue;

                    {
                        std::lock_guard<std::mutex> lock(state.mu);
                        json row;
                        row["coin"] = crypto_coin;
                        row["question"] = entry.market.question;
                        row["up_bid"] = quotes.up_bid;
                        row["up_ask"] = quotes.up_ask;
                        row["down_bid"] = quotes.down_bid;
                        row["down_ask"] = quotes.down_ask;
                        row["deviation_pct"] = md.deviation_pct;
                        row["minutes_remaining"] = md.minutes_remaining;
                        row["market_type"] = crypto_4h_market ? "crypto_4h" : "crypto_daily";
                        state.markets.push_back(row);
                        state.tradable_market_count = static_cast<int>(state.markets.size());
                    }

                    polymarket::ExperimentQuotes exp_quotes;
                    exp_quotes.up_bid = quotes.up_bid;
                    exp_quotes.up_ask = quotes.up_ask;
                    exp_quotes.down_bid = quotes.down_bid;
                    exp_quotes.down_ask = quotes.down_ask;
                    exp_quotes.up_token_id = quotes.up_token_id;
                    exp_quotes.down_token_id = quotes.down_token_id;
                    if (crypto_4h_market) {
                        crypto_4h_experiment.on_market(crypto_coin, md, entry, exp_quotes, now_ms());
                    } else {
                        crypto_daily_experiment.on_market(crypto_coin, md, entry, exp_quotes, now_ms());
                    }
                    continue;
                }
                bool finance_market = coin.rfind("FIN:", 0) == 0;
                std::string finance_asset = finance_market ? coin.substr(4) : "";
                if (finance_market) {
                    polymarket::BtcMarketData md;
                    auto fin_it = finance_market_data.find(cid);
                    if (fin_it == finance_market_data.end()) {
                        try {
                            md = finance_feed.fetch_for_market(entry.market);
                            finance_market_data[cid] = md;
                        } catch (const std::exception& e) {
                            spdlog::debug("Finance feed skipped [{}]: {}", entry.market.question, e.what());
                            continue;
                        }
                    } else {
                        md = fin_it->second;
                    }

                    bool has_open_position = false;
                    std::set<std::string> position_tokens;
                    for (const auto& p : positions) {
                        if (!p.closed && p.condition_id == cid) {
                            has_open_position = true;
                            position_tokens.insert(p.token_id);
                        }
                    }

                    market_feed.apply_cached_quotes(quote_cache, now_ms(), quote_max_age_ms);
                    auto updated = market_feed.get_market(cid);
                    if (!updated) continue;
                    auto quotes = extract_quotes(*updated);
                    auto quote_now = now_ms();
                    bool up_fresh = !quotes.up_token_id.empty() &&
                        quote_cache.is_fresh(quotes.up_token_id, quote_now, quote_max_age_ms);
                    bool down_fresh = !quotes.down_token_id.empty() &&
                        quote_cache.is_fresh(quotes.down_token_id, quote_now, quote_max_age_ms);
                    bool need_rest_quote = !up_fresh || !down_fresh ||
                        quotes.up_ask <= 0 || quotes.down_ask <= 0;
                    if (need_rest_quote) {
                        bool rest_ok = has_open_position
                            ? market_feed.refresh_order_book(cid, position_tokens)
                            : market_feed.refresh_order_book(cid);
                        if (!rest_ok) {
                            reject_agg.add("finance_quote_stale:" + finance_asset, md.deviation_pct);
                            continue;
                        }
                        updated = market_feed.get_market(cid);
                        if (!updated) continue;
                        quotes = extract_quotes(*updated);
                    }
                    if (quotes.up_ask <= 0 || quotes.down_ask <= 0) continue;

                    {
                        std::lock_guard<std::mutex> lock(state.mu);
                        json row;
                        row["coin"] = finance_asset;
                        row["question"] = entry.market.question;
                        row["up_bid"] = quotes.up_bid;
                        row["up_ask"] = quotes.up_ask;
                        row["down_bid"] = quotes.down_bid;
                        row["down_ask"] = quotes.down_ask;
                        row["deviation_pct"] = md.deviation_pct;
                        row["minutes_remaining"] = md.minutes_remaining;
                        state.markets.push_back(row);
                        state.tradable_market_count = static_cast<int>(state.markets.size());
                    }

                    polymarket::ExperimentQuotes exp_quotes;
                    exp_quotes.up_bid = quotes.up_bid;
                    exp_quotes.up_ask = quotes.up_ask;
                    exp_quotes.down_bid = quotes.down_bid;
                    exp_quotes.down_ask = quotes.down_ask;
                    exp_quotes.up_token_id = quotes.up_token_id;
                    exp_quotes.down_token_id = quotes.down_token_id;
                    finance_experiment.on_market(finance_asset, md, entry, exp_quotes, now_ms());
                    continue;
                }
                auto data_it = market_data.find(coin);
                auto strat_it = strategies.find(coin);
                auto cfg_it = coin_cfg.find(coin);
                if (data_it == market_data.end() || strat_it == strategies.end() ||
                    cfg_it == coin_cfg.end()) {
                    continue;
                }
                const auto& md = data_it->second;

                bool has_open_position = false;
                bool has_open_position_for_coin = false;
                std::set<std::string> position_tokens;
                for (const auto& p : positions) {
                    if (!p.closed && p.coin == coin) {
                        has_open_position_for_coin = true;
                    }
                    if (!p.closed && p.condition_id == cid) {
                        has_open_position = true;
                        position_tokens.insert(p.token_id);
                    }
                }
                bool entry_window = md.minutes_remaining > 20;
                if (entry_window) {
                    std::lock_guard<std::mutex> lock(state.mu);
                    state.entry_window_market_count++;
                }
                if (has_open_position) {
                    std::lock_guard<std::mutex> lock(state.mu);
                    state.held_market_count++;
                }
                if (!entry_window && !has_open_position) {
                    reject_agg.add("time_too_short_loop:" + coin, md.deviation_pct);
                    continue;
                }

                market_feed.apply_cached_quotes(quote_cache, now_ms(), quote_max_age_ms);
                auto updated = market_feed.get_market(cid);
                if (!updated) continue;
                auto quotes = extract_quotes(*updated);
                auto quote_now = now_ms();
                bool up_fresh = !quotes.up_token_id.empty() &&
                    quote_cache.is_fresh(quotes.up_token_id, quote_now, quote_max_age_ms);
                bool down_fresh = !quotes.down_token_id.empty() &&
                    quote_cache.is_fresh(quotes.down_token_id, quote_now, quote_max_age_ms);
                bool need_rest_quote = false;
                if (live_trader && has_open_position) {
                    for (const auto& token_id : position_tokens) {
                        if (!quote_cache.is_fresh(token_id, quote_now, quote_max_age_ms)) {
                            need_rest_quote = true;
                            break;
                        }
                    }
                } else if (!up_fresh || !down_fresh || quotes.up_ask <= 0 || quotes.down_ask <= 0) {
                    need_rest_quote = true;
                }

                if (need_rest_quote) {
                    bool rest_ok = live_trader && has_open_position
                        ? market_feed.refresh_order_book(cid, position_tokens)
                        : market_feed.refresh_order_book(cid);
                    if (!rest_ok) {
                        reject_agg.add("quote_stale:" + coin, md.deviation_pct);
                        continue;
                    }
                    updated = market_feed.get_market(cid);
                    if (!updated) continue;
                    quotes = extract_quotes(*updated);
                }

                if ((!live_trader || !has_open_position) && updated->best_prices.size() < 2) continue;
                if ((!live_trader || !has_open_position) &&
                    (quotes.up_ask <= 0 || quotes.down_ask <= 0)) continue;

                last_quotes[coin] = {quotes.up_bid, quotes.up_ask, quotes.down_bid,
                                     quotes.down_ask, entry.market.question};

                // 同步当前 market quotes 到 SharedState（dashboard 显示用）
                {
                    std::lock_guard<std::mutex> lock(state.mu);
                    state.up_bid          = quotes.up_bid;
                    state.up_ask          = quotes.up_ask;
                    state.down_bid        = quotes.down_bid;
                    state.down_ask        = quotes.down_ask;
                    state.market_question = entry.market.question;
                    json row;
                    row["coin"] = coin;
                    row["question"] = entry.market.question;
                    row["up_bid"] = quotes.up_bid;
                    row["up_ask"] = quotes.up_ask;
                    row["down_bid"] = quotes.down_bid;
                    row["down_ask"] = quotes.down_ask;
                    row["deviation_pct"] = md.deviation_pct;
                    row["minutes_remaining"] = md.minutes_remaining;
                    state.markets.push_back(row);
                    state.tradable_market_count = static_cast<int>(state.markets.size());
                }

                spdlog::debug("Market [{}]: {} | Up: {:.3f}/{:.3f} | Down: {:.3f}/{:.3f}",
                              coin, entry.market.question,
                              quotes.up_bid, quotes.up_ask,
                              quotes.down_bid, quotes.down_ask);

                polymarket::ExperimentQuotes exp_quotes;
                exp_quotes.up_bid = quotes.up_bid;
                exp_quotes.up_ask = quotes.up_ask;
                exp_quotes.down_bid = quotes.down_bid;
                exp_quotes.down_ask = quotes.down_ask;
                exp_quotes.up_token_id = quotes.up_token_id;
                exp_quotes.down_token_id = quotes.down_token_id;
                trend_v2_experiment.on_market(coin, md, entry, exp_quotes, now_ms());
                trend_v3_experiment.on_market(coin, md, entry, exp_quotes, now_ms());

                if (!entry_window) {
                    continue;
                }

                if (live_trader && has_open_position) {
                    continue;
                }

                if (!live_trader && has_open_position_for_coin) {
                    reject_agg.add("coin_position_open:" + coin, md.deviation_pct);
                    continue;
                }

                if (live_trader && coin != "BTC") {
                    reject_agg.add("live_btc_only:" + coin, md.deviation_pct);
                    continue;
                }

                if (live_trader && !cfg_it->second.live_enabled) {
                    reject_agg.add("live_coin_disabled:" + coin, md.deviation_pct);
                    continue;
                }

                auto sig = strat_it->second.evaluate_entry(
                    md, quotes.up_bid, quotes.up_ask,
                    quotes.down_bid, quotes.down_ask,
                    quotes.up_token_id, quotes.down_token_id,
                    cid, entry.market.question,
                    md.minutes_remaining, &market_data);

                if (sig.valid) {
                    if (!live_trader && opened_coins_this_tick.count(coin) > 0) {
                        reject_agg.add("coin_opened_this_tick:" + coin, md.deviation_pct);
                        continue;
                    }
                    if (sig.regime == polymarket::StrategyRegime::QUIET_REVERSION) {
                        double token_spread = sig.side == polymarket::Side::UP
                            ? quotes.up_ask - quotes.up_bid
                            : quotes.down_ask - quotes.down_bid;
                        if (token_spread > cfg_it->second.max_spread) {
                            reject_agg.add("quiet_token_spread_wide:" + coin, md.deviation_pct);
                            continue;
                        }
                    }
                    std::string risk_reject;
                    if (risk.can_open_position(sig, risk_reject)) {
                        if (live_trader) {
                            bool rest_ok = market_feed.refresh_order_book(cid);
                            updated = market_feed.get_market(cid);
                            if (!rest_ok || !updated) {
                                spdlog::warn("LIVE entry skipped [{}]: pre-order quote refresh failed",
                                             coin);
                                continue;
                            }
                            quotes = extract_quotes(*updated);
                            auto fresh_sig = strat_it->second.evaluate_entry(
                                md, quotes.up_bid, quotes.up_ask,
                                quotes.down_bid, quotes.down_ask,
                                quotes.up_token_id, quotes.down_token_id,
                                cid, entry.market.question,
                                md.minutes_remaining, &market_data);
                            if (!fresh_sig.valid || fresh_sig.side != sig.side ||
                                fresh_sig.regime != sig.regime ||
                                fresh_sig.token_id != sig.token_id) {
                                spdlog::warn("LIVE entry skipped [{}]: signal changed after quote refresh",
                                             coin);
                                continue;
                            }
                            sig = fresh_sig;
                        }
                        // 交易所最小单（执行层约束，不动策略决策）：限价买单 ≥ min_order_shares 股，
                        // 且名义额 ≥ min_order_usdc（否则离场市价卖单 <$1 现实卖不掉）。份额托到下限后重算 size。
                        double min_shares = std::max(cfg.fees.min_order_shares,
                                                     cfg.fees.min_order_usdc / sig.entry_price);
                        double shares = std::max(sig.shares, min_shares);
                        double size = shares * sig.entry_price;
                        // 入场是限价 ask-1¢ 挂单，maker 角色，免 trading fee
                        double fee = calc_fee(shares, sig.entry_price, /*is_taker=*/false, cfg.fees);

                        polymarket::Position pos;
                        pos.id = "P" + std::to_string(next_position_id++);
                        pos.side = sig.side;
                        pos.regime = sig.regime;
                        pos.coin = sig.coin;
                        pos.token_id = sig.token_id;
                        pos.condition_id = sig.condition_id;
                        pos.market_question = sig.market_question;
                        pos.entry_price = sig.entry_price;
                        pos.current_price = sig.market_ask;
                        pos.size_usdc = size;
                        pos.shares = shares;
                        pos.btc_price_at_entry = md.current_price;
                        pos.btc_strike_at_entry = md.strike_price;
                        pos.entry_vol = md.current_1h_vol;
                        pos.avg_vol = md.avg_24h_vol;
                        pos.entry_fee = fee;
                        pos.entry_time = now_ms();
                        pos.minutes_remaining_at_entry = md.minutes_remaining;
                        pos.tp_levels = strat_it->second.compute_tp_levels(sig.entry_price, sig.regime);

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
                        pos.entry_confidence = sig.entry_confidence;
                        pos.btc_alignment = sig.btc_alignment;
                        pos.eth_alignment = sig.eth_alignment;
                        pos.cross_coin_state = sig.cross_coin_state;
                        pos.entry_price_bucket = sig.entry_price_bucket;
                        pos.confidence_components = sig.confidence_components;

                        // R-V2.6 + P0 fix: LIVE 模式真发 POST /order，然后轮询直到 fill
                        // 关键：BUY 是限价 GTC，可能挂着不立即成交。bot 必须等 fill 确认才创建
                        // paper position，否则后续 SELL 会被 server reject "not enough balance"。
                        if (live_trader) {
                            polymarket::OrderResult buy_result;
                            std::string buy_id;
                            try {
                                buy_result = live_trader->place_entry_order(sig, shares);
                                if (!buy_result.success) {
                                    spdlog::error("LIVE BUY rejected: {} (skip)", buy_result.error);
                                    continue;
                                }
                                buy_id = buy_result.order_id;
                                spdlog::info("LIVE BUY placed order_id={}, polling fill...", buy_id);
                            } catch (const std::exception& e) {
                                spdlog::error("LIVE BUY threw: {} (skip)", e.what());
                                continue;
                            }

                            // 轮询 fill：max 60s, 每 3s 一次 GET /data/order/<id>
                            const int max_wait_sec = 60;
                            const int poll_interval = 3;
                            int waited = 0;
                            bool filled = false;
                            std::string final_status;
                            while (waited < max_wait_sec && g_running) {
                                std::this_thread::sleep_for(std::chrono::seconds(poll_interval));
                                waited += poll_interval;
                                auto d = live_trader->get_order(buy_id);
                                if (!d.ok) {
                                    spdlog::warn("get_order err: {} (continue polling)", d.error);
                                    continue;
                                }
                                final_status = d.status;
                                if (d.status == "matched") { filled = true; break; }
                                if (d.status == "cancelled" || d.status == "killed") break;
                            }

                            if (!filled) {
                                spdlog::warn("LIVE BUY didn't fill in {}s (status={}), cancelling",
                                             waited, final_status);
                                try { live_trader->cancel_order(buy_id); } catch (...) {}
                                continue;  // 不创建 paper position，等下个信号
                            }
                            spdlog::info("LIVE BUY filled in {}s ✓", waited);

                            // R-V2.7-P1: 用真实成交价覆盖逻辑价（POST /order 响应里 makingAmount/takingAmount）
                            // CLOB 限价单常获得优于挂单价的成交（吃到更深 ask），不修正会让策略 P&L 严重偏离真实余额
                            if (buy_result.filled_shares > 0 && buy_result.filled_avg_price > 0) {
                                double real_entry  = buy_result.filled_avg_price;
                                double real_shares = buy_result.filled_shares;
                                spdlog::info("LIVE BUY real fill: shares={:.4f} avg_price={:.4f} (limit was {:.4f}, slippage {:+.4f})",
                                             real_shares, real_entry, sig.entry_price,
                                             sig.entry_price - real_entry);
                                pos.entry_price = real_entry;
                                pos.shares      = real_shares;
                                pos.size_usdc   = real_shares * real_entry;
                                // 重算 V2 maker fee（=0）+ TP/MFE 基线，确保后续止盈与 stop 都按真实成本评估
                                pos.entry_fee   = calc_fee(real_shares, real_entry, /*is_taker=*/false, cfg.fees);
                                pos.tp_levels   = strat_it->second.compute_tp_levels(real_entry, pos.regime);
                                pos.max_price   = real_entry;
                                pos.min_price   = real_entry;
                                pos.mfe_at_5min  = real_entry;
                                pos.mfe_at_10min = real_entry;
                                pos.mfe_at_15min = real_entry;
                                // 同步本地变量，下面 deduct_balance / OPEN 日志用
                                size = pos.size_usdc;
                                fee  = pos.entry_fee;
                                shares = real_shares;
                            } else {
                                spdlog::warn("LIVE BUY filled but no fill amount in response (taking={:.4f}/avg={:.4f}); "
                                             "falling back to logical limit price",
                                             buy_result.filled_shares, buy_result.filled_avg_price);
                            }
                            // fill 完真扣 cash，立刻刷余额（push 给 dashboard）
                            try { live_trader->read_polymarket_balance(); } catch (...) {}
                        }

                        positions.push_back(pos);
                        if (!live_trader) {
                            opened_coins_this_tick.insert(pos.coin);
                        }
                        risk.add_position(pos.coin, pos.regime);
                        risk.deduct_balance(size + fee);  // 动态余额：扣除成本+买入手续费

                        spdlog::info("OPEN [{}] {} [{}:{}] {} @ {:.3f} | ${:.2f} ({:.4f} shares) | fee=${:.2f} | balance=${:.2f}",
                                     cfg.strategy.mode, pos.id, pos.coin, regime_name(pos.regime),
                                     (pos.side == polymarket::Side::UP ? "UP" : "DOWN"),
                                     pos.entry_price, size, shares, fee, display_balance());
                    } else {
                        reject_agg.add(risk_reject, md.deviation_pct);
                    }
                } else if (!sig.reject_reason.empty()) {
                    reject_agg.add(sig.reject_reason, md.deviation_pct);
                    log_main_candidate(coin, md, sig, quotes, market_data);
                }
            }

            // 管理现有持仓
            for (auto& pos : positions) {
                if (pos.closed || !g_running) continue;
                auto pos_data_it = market_data.find(pos.coin);
                if (pos_data_it == market_data.end()) continue;
                const auto& pos_md = pos_data_it->second;
                auto pos_strategy_it = strategies.find(pos.coin);
                if (pos_strategy_it == strategies.end()) continue;

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
                                 last_price, pnl, display_balance());

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
                    fill_analytics(rec, pos, pos_md, cfg.strategy.mode);
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

                // Trail 评估埋点：首次到达 entry+0.05 记武装时刻，之后跟踪武装后最低 bid
                if (pos.armed_at_ms == 0 && current_price >= pos.entry_price + 0.05) {
                    pos.armed_at_ms = now_ms();
                    pos.min_price_after_arm = current_price;
                } else if (pos.armed_at_ms != 0 && current_price < pos.min_price_after_arm) {
                    pos.min_price_after_arm = current_price;
                }

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
                    if (pos.shares_remaining_pct < 0.01) break;  // 已清仓（含 dust 规则）无份额可卖
                    if (current_price >= tp.trigger_price) {
                        double sell_shares = pos.shares * tp.sell_pct * pos.shares_remaining_pct;
                        // 交易所最小单：市价卖单 <$1 现实下不出。若这一档或卖后剩余 <$1，则一次性清掉剩余整仓。
                        double remaining_now = pos.shares * pos.shares_remaining_pct;
                        if (sell_shares * current_price < cfg.fees.min_order_usdc ||
                            (remaining_now - sell_shares) * current_price < cfg.fees.min_order_usdc) {
                            sell_shares = remaining_now;
                        }

                        // LIVE partial TP uses FAK at a 0.01 floor: fill immediately
                        // against available bids, cancel any leftover, and account by real fill.
                        // paper P&L 仍按 current_price 算（与 dry_run 一致），LIVE 实际成交价可能稍低。
                        double real_exit_price = current_price;
                        double real_sell_shares = sell_shares;
                        if (live_trader) {
                            try {
                                auto r = live_trader->place_exit_order(
                                    pos, sell_shares, /*price=*/0.01,
                                    /*is_taker=*/true, "tp" + std::to_string(tp.tier),
                                    /*allow_partial_fill=*/true);
                                if (!r.success) {
                                    spdlog::error("LIVE TP{} SELL rejected: {} (retry next tick)",
                                                 tp.tier, r.error);
                                    continue;
                                }
                                spdlog::info("LIVE TP{} SELL ok order_id={}", tp.tier, r.order_id);
                                // R-V2.7-P1: 真实成交价（floor 0.01 → server 按 best_bid 成交，价格远高于 floor）
                                if (r.filled_shares > 0) {
                                    real_sell_shares = r.filled_shares;
                                    if (r.filled_avg_price > 0) real_exit_price = r.filled_avg_price;
                                    spdlog::info("LIVE TP{} real fill: shares={:.4f} avg_price={:.4f} (book mid was {:.4f})",
                                                 tp.tier, real_sell_shares, real_exit_price, current_price);
                                }
                                // SELL 成交后立即刷余额（dashboard 同步）
                                try { live_trader->read_polymarket_balance(); } catch (...) {}
                            } catch (const std::exception& e) {
                                spdlog::error("LIVE TP{} SELL threw: {} (retry)", tp.tier, e.what());
                                continue;
                            }
                        }

                        tp.triggered = true;
                        double sell_value = real_sell_shares * real_exit_price;
                        double cost_basis = real_sell_shares * pos.entry_price;
                        // TP 卖单在 best_bid 价吃单，taker 角色；入场是 maker，按 0 费摊销
                        double exit_fee = calc_fee(real_sell_shares, real_exit_price, /*is_taker=*/true, cfg.fees);
                        double entry_fee_portion = calc_fee(real_sell_shares, pos.entry_price, /*is_taker=*/false, cfg.fees);
                        double pnl = sell_value - cost_basis - exit_fee - entry_fee_portion;

                        double remaining_before = pos.shares * pos.shares_remaining_pct;
                        double remaining_after = std::max(0.0, remaining_before - real_sell_shares);
                        pos.shares_remaining_pct = pos.shares > 0 ? remaining_after / pos.shares : 0.0;
                        pos.realized_pnl += pnl;
                        risk.add_balance(sell_value - exit_fee);  // 动态余额：回收卖出收入

                        spdlog::info("TP{} [{}]: sell {:.1f} shares @ {:.3f} | pnl=${:+.4f} | remaining={:.0f}% | balance=${:.2f}",
                                     tp.tier, pos.id, real_sell_shares, real_exit_price,
                                     pnl, pos.shares_remaining_pct * 100, display_balance());

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
                        tp_rec.exit_price = real_exit_price;
                        tp_rec.size_usdc = real_sell_shares * pos.entry_price;
                        tp_rec.shares = real_sell_shares;
                        tp_rec.btc_price_at_entry = pos.btc_price_at_entry;
                        tp_rec.btc_strike = pos.btc_strike_at_entry;
                        tp_rec.btc_deviation_pct = (pos.btc_price_at_entry - pos.btc_strike_at_entry) /
                                                     pos.btc_strike_at_entry * 100.0;
                        tp_rec.entry_vol = pos.entry_vol;
                        tp_rec.avg_vol = pos.avg_vol;
                        tp_rec.exit_reason = "tp" + std::to_string(tp.tier);
                        tp_rec.realized_pnl = pnl;
                        tp_rec.fee_paid = exit_fee + entry_fee_portion;
                        fill_analytics(tp_rec, pos, pos_md, cfg.strategy.mode);
                        journal.record(tp_rec);
                    }
                }

                // TP 卖光后自���关闭仓位（TP 子记录已完整记录所有卖出，无需额外 journal 记录）
                if (pos.shares_remaining_pct < 0.01 && !pos.closed) {
                    pos.closed = true;
                    pos.close_reason = "tp_filled";
                    risk.remove_position();

                    spdlog::info("ALL TP FILLED [{}]: total_rpnl=${:+.2f} | balance=${:.2f}",
                                 pos.id, pos.realized_pnl, display_balance());
                    continue;
                }

                // 检查止损
                auto exit_sig = pos_strategy_it->second.evaluate_exit(
                    pos, current_price, pos_md, pos_md.minutes_remaining);
                if (exit_sig.should_exit) {
                    double remaining_shares = pos.shares * pos.shares_remaining_pct;

                    // R-V2.6 + P1 fix: LIVE FOK SELL 用 floor 0.01（让 server 按 best_bid 吃）
                    double real_exit_price = exit_sig.exit_price;
                    double real_exit_shares = remaining_shares;
                    if (live_trader) {
                        try {
                            auto r = live_trader->place_exit_order(
                                pos, remaining_shares, /*price=*/0.01,
                                /*is_taker=*/true, exit_sig.reason);
                            if (!r.success) {
                                spdlog::error("LIVE STOP SELL rejected: {} (retry next tick)", r.error);
                                continue;
                            }
                            spdlog::info("LIVE STOP SELL ok order_id={}", r.order_id);
                            // R-V2.7-P1: 真实成交价（FOK floor 0.01 → server 按 best_bid 成交）
                            if (r.filled_shares > 0) {
                                real_exit_shares = r.filled_shares;
                                if (r.filled_avg_price > 0) real_exit_price = r.filled_avg_price;
                                spdlog::info("LIVE STOP real fill: shares={:.4f} avg_price={:.4f} (strategy mid was {:.4f})",
                                             real_exit_shares, real_exit_price, exit_sig.exit_price);
                            }
                            // SELL 成交后立即刷余额
                            try { live_trader->read_polymarket_balance(); } catch (...) {}
                        } catch (const std::exception& e) {
                            spdlog::error("LIVE STOP SELL threw: {} (retry)", e.what());
                            continue;
                        }
                    }

                    double sell_value = real_exit_shares * real_exit_price;
                    double cost_basis = real_exit_shares * pos.entry_price;
                    // 止损/拖尾/时间止损都是吃 best_bid，taker；入场 maker
                    double exit_fee = calc_fee(real_exit_shares, real_exit_price, /*is_taker=*/true, cfg.fees);
                    double entry_fee_portion = calc_fee(real_exit_shares, pos.entry_price, /*is_taker=*/false, cfg.fees);
                    double pnl = sell_value - cost_basis - exit_fee - entry_fee_portion;

                    pos.realized_pnl += pnl;
                    pos.closed = true;
                    pos.close_reason = exit_sig.reason;
                    risk.remove_position();
                    risk.add_balance(sell_value - exit_fee);  // 动态余额：回收卖出收入

                    // §三 止损后本场不再交易（价格止损 / 时间止损 / 移动止盈回撤 / 死水早退都算止损出场）
                    if (exit_sig.reason == "stop_price" || exit_sig.reason == "stop_time" ||
                        exit_sig.reason == "stop_btc" ||
                        exit_sig.reason == "trailing_stop" || exit_sig.reason == "dead_water_exit" ||
                        exit_sig.reason == "fast_fail_exit" ||
                        exit_sig.reason == "cheap_fail_stop" ||
                        exit_sig.reason == "adverse_expansion_stop") {
                        risk.set_candle_stopped(pos.coin);
                        if (exit_sig.reason == "stop_price") {
                            risk.record_stop_price(pos.coin);
                        }
                        spdlog::warn("Candle stopped [{}]: {} triggered, no more trades this candle",
                                     pos.coin, exit_sig.reason);
                    }

                    if (pnl > 0) risk.record_profit(pnl);
                    else risk.record_loss(-pnl);

                    spdlog::info("CLOSE [{}] {} reason={} | pnl=${:+.2f} | balance=${:.2f}",
                                 pos.id,
                                 (pos.side == polymarket::Side::UP ? "UP" : "DOWN"),
                                 exit_sig.reason, pnl, display_balance());

                    polymarket::TradeRecord rec;
                    rec.id = pos.id;
                    rec.market_question = pos.market_question;
                    rec.side = (pos.side == polymarket::Side::UP) ? "UP" : "DOWN";
                    rec.entry_time = pos.entry_time;
                    rec.exit_time = now_ms();
                    rec.minutes_remaining_at_entry = pos.minutes_remaining_at_entry;
                    rec.entry_price = pos.entry_price;
                    rec.exit_price = real_exit_price;
                    rec.size_usdc = real_exit_shares * pos.entry_price;
                    rec.shares = real_exit_shares;
                    rec.btc_price_at_entry = pos.btc_price_at_entry;
                    rec.btc_strike = pos.btc_strike_at_entry;
                    rec.btc_deviation_pct = (pos.btc_price_at_entry - pos.btc_strike_at_entry) /
                                             pos.btc_strike_at_entry * 100.0;
                    rec.entry_vol = pos.entry_vol;
                    rec.avg_vol = pos.avg_vol;
                    rec.exit_reason = exit_sig.reason;
                    rec.realized_pnl = pnl;  // 只记本次卖出的 P&L，不是累积
                    rec.fee_paid = exit_fee + entry_fee_portion;
                    fill_analytics(rec, pos, pos_md, cfg.strategy.mode);

                    // §五.1b dead_water 埋点（Step 2.23）：触发瞬间的市场上下文
                    // 用于 20+ 笔后回归是否要加 BTC 方向 / vol 条件
                    if (exit_sig.reason == "dead_water_exit") {
                        rec.dw_btc_dev_pct = pos_md.deviation_pct;
                        rec.dw_vol_ratio   = (pos_md.avg_24h_vol > 1e-9)
                                             ? (pos_md.current_1h_vol / pos_md.avg_24h_vol) : 0.0;
                        // dev_favors：BTC dev 方向是否帮持仓方向获胜
                        // UP 仓位希望 BTC > strike (dev>0)；DOWN 仓位希望 BTC < strike (dev<0)
                        rec.dw_dev_favors = (pos.side == polymarket::Side::UP && pos_md.deviation_pct > 0) ||
                                            (pos.side == polymarket::Side::DOWN && pos_md.deviation_pct < 0);
                        // 触发瞬间 spread（我方 token 的 ask - bid）
                        auto bp_it = updated->best_prices.find(pos.token_id);
                        if (bp_it != updated->best_prices.end()) {
                            rec.dw_spread = bp_it->second.best_ask - bp_it->second.best_bid;
                        }
                    }

                    journal.record(rec);
                }
            }

            // 清理已关闭持仓
            positions.erase(
                std::remove_if(positions.begin(), positions.end(),
                               [](const polymarket::Position& p) { return p.closed; }),
                positions.end());
            finance_experiment.prune_closed();
            crypto_4h_experiment.prune_closed();
            crypto_daily_experiment.prune_closed();

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
                sleep_sec = 5;   // Step 2.25：持仓时 10s→5s，缩短 stop_price 触发 lag
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
                std::string market_bits;
                for (const auto& [coin, md] : state.coins) {
                    auto qit = last_quotes.find(coin);
                    if (!market_bits.empty()) market_bits += " | ";
                    if (qit != last_quotes.end()) {
                        market_bits += fmt::format("{} ${:.2f} {:+.2f}% U{:.2f}/D{:.2f}",
                                                   coin, md.current_price, md.deviation_pct,
                                                   qit->second.up_ask, qit->second.down_ask);
                    } else {
                        market_bits += fmt::format("{} ${:.2f} {:+.2f}%",
                                                   coin, md.current_price, md.deviation_pct);
                    }
                }
                spdlog::info("#{} | {}min | {} | idle | WS:{}:{} | ${:.2f} | {}s",
                             state.tick_count, mins, market_bits,
                             state.clob_ws_connected ? "on" : "off",
                             state.clob_ws_subscribed,
                             display_balance(), sleep_sec);
            } else {
                for (const auto& p : positions) {
                    double chg_pct = p.entry_price > 0 ? (p.current_price - p.entry_price) / p.entry_price * 100 : 0;
                    auto md_it = state.coins.find(p.coin);
                    double px = md_it != state.coins.end() ? md_it->second.current_price : 0;
                    double dev = md_it != state.coins.end() ? md_it->second.deviation_pct : 0;
                    spdlog::info("#{} | {}min | {} ${:.2f} {:+.2f}% | {} [{}:{}] {} {:.2f}->{:.2f} {:+.0f}% MFE:{:.2f} rem:{:.0f}% | ${:.2f} | {}s",
                                 state.tick_count, mins,
                                 p.coin, px, dev,
                                 p.id, p.coin, regime_name(p.regime),
                                 (p.side == polymarket::Side::UP ? "UP" : "DN"),
                                 p.entry_price, p.current_price, chg_pct,
                                 p.max_price, p.shares_remaining_pct * 100,
                                 display_balance(), sleep_sec);
                }
            }
        }

        for (int i = 0; i < sleep_sec && g_running; i++) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    spdlog::info("Shutting down...");

    // R-V2.6: SIGINT/SIGTERM 兜底 — 紧急平所有未平仓位（cancel open orders + SELL FOK）
    if (live_trader && !positions.empty()) {
        spdlog::warn("LIVE: triggering emergency_close_all on {} open position(s)",
                     positions.size());
        try {
            live_trader->emergency_close_all(positions);
        } catch (const std::exception& e) {
            spdlog::error("emergency_close_all threw: {}", e.what());
        }
    }

    clob_ws.stop();
    api.stop();
    journal.print_summary();
    spdlog::info("Done.");
    return 0;
}
