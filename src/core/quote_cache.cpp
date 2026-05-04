#include "core/quote_cache.h"

namespace polymarket {

void QuoteCache::update_book(const OrderBook& book, const BestBidAsk& best,
                             int64_t updated_ms, bool from_ws) {
    if (book.asset_id.empty()) return;
    std::lock_guard<std::mutex> lock(mu_);
    auto& snap = quotes_[book.asset_id];
    snap.book = book;
    snap.best = best;
    snap.best.token_id = book.asset_id;
    snap.updated_ms = updated_ms;
    snap.has_book = true;
    snap.from_ws = from_ws;
}

void QuoteCache::update_best(const std::string& token_id, double best_bid, double best_ask,
                             double bid_size, double ask_size, int64_t updated_ms,
                             bool from_ws) {
    if (token_id.empty()) return;
    std::lock_guard<std::mutex> lock(mu_);
    auto& snap = quotes_[token_id];
    snap.best.token_id = token_id;
    snap.best.best_bid = best_bid;
    snap.best.best_ask = best_ask;
    snap.best.bid_size = bid_size;
    snap.best.ask_size = ask_size;
    snap.updated_ms = updated_ms;
    snap.from_ws = from_ws;
}

bool QuoteCache::get(const std::string& token_id, QuoteSnapshot& out) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = quotes_.find(token_id);
    if (it == quotes_.end()) return false;
    out = it->second;
    return true;
}

bool QuoteCache::get_best(const std::string& token_id, BestBidAsk& out,
                          int64_t& updated_ms) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = quotes_.find(token_id);
    if (it == quotes_.end()) return false;
    out = it->second.best;
    updated_ms = it->second.updated_ms;
    return true;
}

bool QuoteCache::is_fresh(const std::string& token_id, int64_t now_ms,
                          int64_t max_age_ms) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = quotes_.find(token_id);
    if (it == quotes_.end() || it->second.updated_ms <= 0) return false;
    return now_ms - it->second.updated_ms <= max_age_ms;
}

size_t QuoteCache::size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return quotes_.size();
}

}  // namespace polymarket
