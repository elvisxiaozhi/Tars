#include "core/market_feed.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

#include <json.hpp>
#include <spdlog/spdlog.h>

#include "utils/json_helpers.h"

namespace polymarket {

using json = nlohmann::json;
namespace jh = json_helpers;

MarketFeed::MarketFeed(const AppConfig& cfg)
    : cfg_(cfg), http_(10, cfg.network.proxy_url) {}

void MarketFeed::fetch_markets() {
    if (!cfg_.strategy.market_filter.empty()) {
        // gamma 路径：增量拉取，不清空
        fetch_from_gamma();
    } else {
        // CLOB 全量路径：每次清空重拉
        markets_.clear();
        fetch_from_clob();
        scan_prices();
    }
}

void MarketFeed::fetch_from_clob() {
    spdlog::info("Fetching markets from CLOB...");

    std::string cursor;
    int total_fetched = 0;

    while (true) {
        std::string url = cfg_.polymarket.clob_rest_url + "/sampling-markets?limit=500";
        if (!cursor.empty()) {
            url += "&next_cursor=" + cursor;
        }

        auto resp = http_.get(url);
        if (resp.status_code != 200) {
            spdlog::warn("Markets fetch returned status={}, stopping", resp.status_code);
            break;
        }

        auto j = json::parse(resp.body);
        auto page_markets = jh::parse_markets_response(j);
        total_fetched += page_markets.size();

        for (auto& m : page_markets) {
            if (!m.active || m.closed || !m.accepting_orders) continue;
            if (m.tokens.size() < 2) continue;

            MarketEntry entry;
            entry.market = std::move(m);
            markets_[entry.market.condition_id] = std::move(entry);
        }

        cursor = jh::get_string(j, "next_cursor");
        if (cursor.empty() || page_markets.empty()) break;
    }

    spdlog::info("Markets loaded: {} active out of {} total",
                 markets_.size(), total_fetched);
}

// 构造当前小时的 BTC Up/Down event slug
// 格式: bitcoin-up-or-down-{month}-{day}-{year}-{hour}{am/pm}-et
static std::string build_btc_hourly_slug(int offset_hours = 0) {
    auto now = std::chrono::system_clock::now() +
               std::chrono::hours(offset_hours);
    auto tt = std::chrono::system_clock::to_time_t(now);

    // 转换为 ET (UTC-4)
    struct tm et_tm;
    time_t et_time = tt - 4 * 3600;
    gmtime_r(&et_time, &et_tm);

    static const char* months[] = {
        "january", "february", "march", "april", "may", "june",
        "july", "august", "september", "october", "november", "december"
    };

    int hour12 = et_tm.tm_hour % 12;
    if (hour12 == 0) hour12 = 12;
    std::string ampm = (et_tm.tm_hour < 12) ? "am" : "pm";

    char buf[128];
    snprintf(buf, sizeof(buf), "bitcoin-up-or-down-%s-%d-%d-%d%s-et",
             months[et_tm.tm_mon], et_tm.tm_mday,
             et_tm.tm_year + 1900, hour12, ampm.c_str());
    return buf;
}

void MarketFeed::fetch_from_gamma() {
    // 拉取当前小时和下一个小时的市场（跳过已加载的）
    int new_count = 0;

    for (int offset = 0; offset <= 1; offset++) {
        auto slug = build_btc_hourly_slug(offset);

        // 检查是否已经加载过这个市场
        bool already_loaded = false;
        for (const auto& [cid, entry] : markets_) {
            if (entry.market.market_slug == slug) {
                already_loaded = true;
                break;
            }
        }
        if (already_loaded) continue;

        std::string url = cfg_.polymarket.gamma_api_url + "/events?slug=" + slug;
        spdlog::info("Querying gamma: {}", slug);
        auto resp = http_.get(url);
        if (resp.status_code != 200) {
            spdlog::warn("Gamma fetch status={} for {}", resp.status_code, slug);
            continue;
        }

        auto j = json::parse(resp.body);
        if (!j.is_array() || j.empty()) continue;

        for (const auto& event : j) {
            if (!event.contains("markets") || !event["markets"].is_array()) continue;
            for (const auto& mj : event["markets"]) {
                auto m = jh::parse_gamma_market(mj);
                if (!m.active || m.closed) continue;
                if (m.tokens.size() < 2) continue;

                spdlog::info("  new market: {} (tokens: {})", m.question, m.tokens.size());
                MarketEntry entry;
                entry.market = std::move(m);
                markets_[entry.market.condition_id] = std::move(entry);
                new_count++;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    // 移除已关闭的市场（到期后 gamma 会标记 closed）
    for (auto it = markets_.begin(); it != markets_.end(); ) {
        if (it->second.market.closed) {
            spdlog::info("  removing closed market: {}", it->second.market.question);
            it = markets_.erase(it);
        } else {
            ++it;
        }
    }

    if (new_count > 0) {
        spdlog::info("Markets: {} new, {} total", new_count, markets_.size());
    }
}

void MarketFeed::scan_prices() {
    // 遍历所有市场，用 token 自带的价格计算 price_sum
    // price_sum < 1.0 → 可能有套利空间（需拉订单簿确认）
    // price_sum == 1.0 → 正常定价
    // price_sum > 1.0 → 反向套利（卖两边）

    struct PriceScan {
        std::string condition_id;
        std::string question;
        double price_sum;
        double yes_price;
        double no_price;
    };
    std::vector<PriceScan> scans;

    for (const auto& [cid, entry] : markets_) {
        if (entry.market.tokens.size() != 2) continue;

        double yes_p = 0, no_p = 0;
        for (const auto& t : entry.market.tokens) {
            if (t.outcome == "Yes") yes_p = t.price;
            else no_p = t.price;
        }

        double sum = yes_p + no_p;
        scans.push_back({cid, entry.market.question, sum, yes_p, no_p});
    }

    // 按 price_sum 排序
    std::sort(scans.begin(), scans.end(),
              [](const PriceScan& a, const PriceScan& b) {
                  return a.price_sum < b.price_sum;
              });

    // 统计分布
    int below_1 = 0, at_1 = 0, above_1 = 0;
    for (const auto& s : scans) {
        if (s.price_sum < 0.995) below_1++;
        else if (s.price_sum > 1.005) above_1++;
        else at_1++;
    }

    spdlog::info("Price scan: {} markets | sum<1.0: {} | sum~1.0: {} | sum>1.0: {}",
                 scans.size(), below_1, at_1, above_1);

    // 显示 price_sum 最低的 20 个
    spdlog::info("");
    spdlog::info("=== Top candidates by token price sum (lowest first) ===");
    spdlog::info("{:<55} {:>7} {:>7} {:>9}",
                 "Question", "Yes", "No", "Sum");
    spdlog::info("{}", std::string(82, '-'));

    int shown = 0;
    for (const auto& s : scans) {
        if (shown >= 20) break;
        std::string q = s.question;
        if (q.size() > 55) q = q.substr(0, 52) + "...";
        spdlog::info("{:<55} {:>7.3f} {:>7.3f} {:>9.3f}{}",
                     q, s.yes_price, s.no_price, s.price_sum,
                     s.price_sum < 0.995 ? " ***" : "");
        shown++;
    }

    // 收集候选：price_sum 最低的 N 个市场（已按升序排序）
    arb_candidates_.clear();
    int max_scan = cfg_.strategy.max_markets_to_scan;
    for (const auto& s : scans) {
        if (static_cast<int>(arb_candidates_.size()) >= max_scan) break;
        arb_candidates_.push_back(s.condition_id);
    }

    spdlog::info("");
    spdlog::info("{} markets selected for order book fetch ({} with sum<1.0)",
                 arb_candidates_.size(), below_1);
}

int MarketFeed::fetch_order_books() {
    if (arb_candidates_.empty()) {
        spdlog::info("No candidates for order book fetch");
        return 0;
    }

    int total_tokens = 0;
    for (const auto& cid : arb_candidates_) {
        auto it = markets_.find(cid);
        if (it != markets_.end()) total_tokens += it->second.market.tokens.size();
    }

    spdlog::info("Fetching order books for {} tokens across {} markets...",
                 total_tokens, arb_candidates_.size());

    int success = 0;
    int failed = 0;
    auto start = std::chrono::steady_clock::now();

    for (size_t ci = 0; ci < arb_candidates_.size(); ci++) {
        auto it = markets_.find(arb_candidates_[ci]);
        if (it == markets_.end()) continue;
        auto& entry = it->second;

        for (const auto& token : entry.market.tokens) {
            try {
                std::string url = cfg_.polymarket.clob_rest_url +
                                  "/book?token_id=" + token.token_id;
                auto resp = http_.get(url);

                if (resp.status_code == 200) {
                    auto j = json::parse(resp.body);
                    auto ob = jh::parse_order_book(j);
                    auto bba = jh::extract_best_bid_ask(ob);

                    entry.order_books[token.token_id] = std::move(ob);
                    entry.best_prices[token.token_id] = bba;
                    success++;
                } else if (resp.status_code == 429) {
                    spdlog::warn("Rate limited, backing off 2s...");
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                    failed++;
                } else {
                    failed++;
                }
            } catch (const std::exception& e) {
                spdlog::debug("Order book error: {}", e.what());
                failed++;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }

        if ((ci + 1) % 25 == 0) {
            spdlog::info("  progress: {}/{} markets", ci + 1, arb_candidates_.size());
        }
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start);

    spdlog::info("Order books: {} success, {} failed, {}s elapsed",
                 success, failed, elapsed.count());

    return success;
}

bool MarketFeed::refresh_order_book(const std::string& condition_id) {
    auto it = markets_.find(condition_id);
    if (it == markets_.end()) return false;
    auto& entry = it->second;

    int success = 0;
    for (const auto& token : entry.market.tokens) {
        try {
            std::string url = cfg_.polymarket.clob_rest_url +
                              "/book?token_id=" + token.token_id;
            auto resp = http_.get(url);
            if (resp.status_code == 200) {
                auto j = json::parse(resp.body);
                auto ob = jh::parse_order_book(j);
                auto bba = jh::extract_best_bid_ask(ob);
                entry.order_books[token.token_id] = std::move(ob);
                entry.best_prices[token.token_id] = bba;
                success++;
            }
        } catch (const std::exception& e) {
            spdlog::debug("Order book refresh error: {}", e.what());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    return success == static_cast<int>(entry.market.tokens.size());
}

const MarketEntry* MarketFeed::get_market(const std::string& condition_id) const {
    auto it = markets_.find(condition_id);
    if (it != markets_.end()) return &it->second;
    return nullptr;
}

void MarketFeed::print_summary() const {
    struct Row {
        std::string question;
        std::string yes_str;
        std::string no_str;
        double yes_ask;
        double no_ask;
        double spread;
    };
    std::vector<Row> rows;

    for (const auto& [cid, entry] : markets_) {
        if (entry.best_prices.size() < 2) continue;
        if (entry.market.tokens.size() < 2) continue;

        Row row;
        row.question = entry.market.question;
        if (row.question.size() > 55) row.question = row.question.substr(0, 52) + "...";

        row.yes_ask = 0;
        row.no_ask = 0;

        for (const auto& token : entry.market.tokens) {
            auto it = entry.best_prices.find(token.token_id);
            if (it == entry.best_prices.end()) continue;
            const auto& bba = it->second;

            char buf[64];
            snprintf(buf, sizeof(buf), "%.3f/%.3f", bba.best_bid, bba.best_ask);

            if (token.outcome == "Yes") {
                row.yes_str = buf;
                row.yes_ask = bba.best_ask;
            } else {
                row.no_str = buf;
                row.no_ask = bba.best_ask;
            }
        }

        if (row.yes_ask > 0 && row.no_ask > 0) {
            row.spread = (row.yes_ask + row.no_ask - 1.0) * 100;
        } else {
            row.spread = 999.0;
        }

        rows.push_back(std::move(row));
    }

    std::sort(rows.begin(), rows.end(),
              [](const Row& a, const Row& b) { return a.spread < b.spread; });

    spdlog::info("");
    spdlog::info("=== Order Book Quotes ({} markets) ===", rows.size());
    spdlog::info("{:<55} {:>12} {:>12} {:>8}",
                 "Question", "Yes B/A", "No B/A", "Spread");
    spdlog::info("{}", std::string(90, '-'));

    int shown = 0;
    for (const auto& row : rows) {
        std::string tag = row.spread < 0 ? " <-- ARB" : "";
        spdlog::info("{:<55} {:>12} {:>12} {:>+7.1f}%{}",
                     row.question, row.yes_str, row.no_str,
                     row.spread, tag);
        if (++shown >= 30) {
            spdlog::info("  ... ({} more)", static_cast<int>(rows.size()) - shown);
            break;
        }
    }

    int arb_count = 0;
    for (const auto& row : rows) {
        if (row.spread < 0) arb_count++;
    }

    spdlog::info("");
    if (arb_count > 0) {
        spdlog::info(">>> {} potential arbitrage opportunities (ask_yes + ask_no < $1.00) <<<",
                     arb_count);
    } else {
        spdlog::info("No arbitrage opportunities found in order books");
    }
}

}  // namespace polymarket
