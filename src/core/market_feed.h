#pragma once

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "core/quote_cache.h"
#include "utils/config.h"
#include "utils/types.h"
#include "net/http_client.h"

namespace polymarket {

struct MarketEntry {
    Market market;
    std::map<std::string, OrderBook> order_books;   // token_id -> OrderBook
    std::map<std::string, BestBidAsk> best_prices;  // token_id -> BestBidAsk
};

class MarketFeed {
public:
    explicit MarketFeed(const AppConfig& cfg);

    // 拉取市场列表（有 market_filter 时走 gamma API，否则走 CLOB 全量）
    void fetch_markets();
    void fetch_markets(const std::vector<CoinStrategyConfig>& coins);
    void fetch_finance_markets(int limit = 40);
    void fetch_crypto_duration_markets(const std::string& duration, int limit = 80);

    // 为已加载的市场拉取订单簿，返回成功拉取的数量
    int fetch_order_books();

    // 刷新单个市场的订单簿
    bool refresh_order_book(const std::string& condition_id);
    bool refresh_order_book(const std::string& condition_id,
                            const std::set<std::string>& token_ids);
    void apply_cached_quotes(const QuoteCache& cache, int64_t now_ms, int64_t max_age_ms);
    std::set<std::string> active_token_ids() const;

    // 获取所有已加载市场
    const std::map<std::string, MarketEntry>& markets() const { return markets_; }

    // 获取单个市场
    const MarketEntry* get_market(const std::string& condition_id) const;

    // 打印报价摘要
    void print_summary() const;

    size_t market_count() const { return markets_.size(); }

private:
    void fetch_from_clob();   // CLOB 全量拉取
    void fetch_from_gamma();  // gamma API 按 filter 拉取
    void fetch_from_gamma(const std::vector<CoinStrategyConfig>& coins);
    void scan_prices();       // 用 token price 做初筛

    AppConfig cfg_;
    net::HttpClient http_;
    std::map<std::string, MarketEntry> markets_;        // condition_id -> MarketEntry
    std::vector<std::string> arb_candidates_;           // 待拉订单簿的市场
    std::map<std::string, int64_t> gamma_retry_after_ms_; // slug -> next retry time
};

}  // namespace polymarket
