#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

#include "utils/types.h"

namespace polymarket {

struct QuoteSnapshot {
    BestBidAsk best;
    OrderBook book;
    int64_t updated_ms = 0;
    bool has_book = false;
    bool from_ws = false;
};

class QuoteCache {
public:
    void update_book(const OrderBook& book, const BestBidAsk& best, int64_t updated_ms,
                     bool from_ws);
    void update_best(const std::string& token_id, double best_bid, double best_ask,
                     double bid_size, double ask_size, int64_t updated_ms, bool from_ws);

    bool get(const std::string& token_id, QuoteSnapshot& out) const;
    bool get_best(const std::string& token_id, BestBidAsk& out, int64_t& updated_ms) const;
    bool is_fresh(const std::string& token_id, int64_t now_ms, int64_t max_age_ms) const;

    size_t size() const;

private:
    mutable std::mutex mu_;
    std::map<std::string, QuoteSnapshot> quotes_;
};

}  // namespace polymarket
