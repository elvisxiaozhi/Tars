#pragma once

#include <string>
#include <vector>

#include <json.hpp>

#include "utils/types.h"

namespace polymarket::json_helpers {

using json = nlohmann::json;

// --- 安全取值 (缺失字段不抛异常) ---

inline std::string get_string(const json& j, const std::string& key,
                               const std::string& def = "") {
    if (j.contains(key) && !j[key].is_null()) return j[key].get<std::string>();
    return def;
}

inline double get_double(const json& j, const std::string& key, double def = 0.0) {
    if (!j.contains(key) || j[key].is_null()) return def;
    if (j[key].is_string()) {
        try { return std::stod(j[key].get<std::string>()); }
        catch (...) { return def; }
    }
    return j[key].get<double>();
}

inline bool get_bool(const json& j, const std::string& key, bool def = false) {
    if (j.contains(key) && !j[key].is_null()) return j[key].get<bool>();
    return def;
}

inline int get_int(const json& j, const std::string& key, int def = 0) {
    if (j.contains(key) && !j[key].is_null()) return j[key].get<int>();
    return def;
}

// --- 解析 Polymarket 数据结构 ---

inline PriceLevel parse_price_level(const json& j) {
    return {get_double(j, "price"), get_double(j, "size")};
}

inline Token parse_token(const json& j) {
    Token t;
    t.token_id = get_string(j, "token_id");
    t.outcome = get_string(j, "outcome");
    t.price = get_double(j, "price");
    t.winner = get_bool(j, "winner");
    return t;
}

inline Market parse_market(const json& j) {
    Market m;
    m.condition_id = get_string(j, "condition_id");
    m.question_id = get_string(j, "question_id");
    m.question = get_string(j, "question");
    m.description = get_string(j, "description");
    m.market_slug = get_string(j, "market_slug");
    m.active = get_bool(j, "active");
    m.closed = get_bool(j, "closed");
    m.accepting_orders = get_bool(j, "accepting_orders");
    m.neg_risk = get_bool(j, "neg_risk");
    m.neg_risk_market_id = get_string(j, "neg_risk_market_id");
    m.minimum_order_size = get_double(j, "minimum_order_size");
    m.minimum_tick_size = get_double(j, "minimum_tick_size");
    m.seconds_delay = get_int(j, "seconds_delay");

    if (j.contains("tokens") && j["tokens"].is_array()) {
        for (const auto& tj : j["tokens"]) {
            m.tokens.push_back(parse_token(tj));
        }
    }
    if (j.contains("tags") && j["tags"].is_array()) {
        for (const auto& tag : j["tags"]) {
            if (tag.is_string()) m.tags.push_back(tag.get<std::string>());
        }
    }
    return m;
}

inline std::vector<Market> parse_markets_response(const json& j) {
    std::vector<Market> markets;
    // CLOB API: {"data": [...]}  或  直接 [...]
    const auto& arr = j.contains("data") ? j["data"] : j;
    if (arr.is_array()) {
        for (const auto& mj : arr) {
            markets.push_back(parse_market(mj));
        }
    }
    return markets;
}

inline OrderBook parse_order_book(const json& j) {
    OrderBook ob;
    ob.market = get_string(j, "market");
    ob.asset_id = get_string(j, "asset_id");
    ob.timestamp = get_string(j, "timestamp");
    ob.hash = get_string(j, "hash");

    if (j.contains("bids") && j["bids"].is_array()) {
        for (const auto& b : j["bids"]) {
            ob.bids.push_back(parse_price_level(b));
        }
    }
    if (j.contains("asks") && j["asks"].is_array()) {
        for (const auto& a : j["asks"]) {
            ob.asks.push_back(parse_price_level(a));
        }
    }
    return ob;
}

// 从订单簿提取 best bid/ask
inline BestBidAsk extract_best_bid_ask(const OrderBook& ob) {
    BestBidAsk bba;
    bba.token_id = ob.asset_id;
    if (!ob.bids.empty()) {
        bba.best_bid = ob.bids.front().price;
        bba.bid_size = ob.bids.front().size;
    }
    if (!ob.asks.empty()) {
        bba.best_ask = ob.asks.front().price;
        bba.ask_size = ob.asks.front().size;
    }
    return bba;
}

// --- 解析 WebSocket 事件 ---

inline WsPriceChange parse_ws_price_change(const json& j) {
    WsPriceChange pc;
    pc.asset_id = get_string(j, "asset_id");
    pc.price = get_double(j, "price");
    pc.size = get_double(j, "size");
    pc.side = get_string(j, "side");
    return pc;
}

inline WsLastTradePrice parse_ws_last_trade(const json& j) {
    WsLastTradePrice lt;
    lt.asset_id = get_string(j, "asset_id");
    lt.price = get_double(j, "price");
    lt.size = get_double(j, "size");
    lt.side = get_string(j, "side");
    return lt;
}

}  // namespace polymarket::json_helpers
