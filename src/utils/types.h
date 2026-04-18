#pragma once

#include <string>
#include <vector>

namespace polymarket {

// --- 基础类型 ---

struct PriceLevel {
    double price = 0.0;
    double size = 0.0;
};

struct Token {
    std::string token_id;
    std::string outcome;   // "Yes" / "No" / 选项名
    double price = 0.0;
    bool winner = false;
};

struct Market {
    std::string condition_id;
    std::string question_id;
    std::string question;
    std::string description;
    std::string market_slug;
    bool active = false;
    bool closed = false;
    bool accepting_orders = false;
    bool neg_risk = false;
    std::string neg_risk_market_id;
    double minimum_order_size = 0.0;
    double minimum_tick_size = 0.0;
    int seconds_delay = 0;
    std::vector<Token> tokens;
    std::vector<std::string> tags;
};

// --- 订单簿 ---

struct OrderBook {
    std::string market;     // condition_id
    std::string asset_id;   // token_id
    std::string timestamp;
    std::string hash;
    std::vector<PriceLevel> bids;  // 降序 (最高买价在前)
    std::vector<PriceLevel> asks;  // 升序 (最低卖价在前)
};

// --- WebSocket 事件 ---

struct WsPriceChange {
    std::string asset_id;
    double price = 0.0;
    double size = 0.0;
    std::string side;     // "BUY" / "SELL"
};

struct WsLastTradePrice {
    std::string asset_id;
    double price = 0.0;
    double size = 0.0;
    std::string side;
};

// 聚合的 best bid/ask（套利检测用）
struct BestBidAsk {
    std::string token_id;
    double best_bid = 0.0;
    double best_ask = 0.0;
    double bid_size = 0.0;
    double ask_size = 0.0;
};

}  // namespace polymarket
