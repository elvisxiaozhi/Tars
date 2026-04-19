#pragma once

#include <string>
#include <vector>

#include "net/http_client.h"

namespace polymarket {

struct BtcCandle {
    int64_t open_time;    // unix ms
    int64_t close_time;   // unix ms
    double open;
    double high;
    double low;
    double close;
    double volume;
};

struct BtcMarketData {
    double current_price = 0;
    double strike_price = 0;       // 当前1h K线的 open
    double current_1h_vol = 0;     // 当前1h波动率 (high-low)/open
    double avg_24h_vol = 0;        // 近24小时平均1h波动率
    bool price_above_strike = false;
    double deviation_pct = 0;      // (current - strike) / strike * 100
    int64_t candle_open_time = 0;
    int64_t candle_close_time = 0;
    int minutes_into_candle = 0;   // 当前K线已过去的分钟数
    int minutes_remaining = 0;     // 当前K线剩余分钟数
};

class BinanceFeed {
public:
    explicit BinanceFeed(const std::string& proxy_url = "");

    // 拉取最新 BTC 市场数据（价格 + K线 + 波动率）
    BtcMarketData fetch();

    // 获取最近一次 fetch 的数据
    const BtcMarketData& latest() const { return latest_; }

private:
    double fetch_price();
    std::vector<BtcCandle> fetch_klines(int limit = 25);
    BtcMarketData compute(double price, const std::vector<BtcCandle>& klines);

    net::HttpClient http_;
    BtcMarketData latest_;
    static constexpr const char* BINANCE_API = "https://api.binance.com";
};

}  // namespace polymarket
