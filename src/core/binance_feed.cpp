#include "core/binance_feed.h"

#include <chrono>
#include <cmath>
#include <numeric>

#include <json.hpp>
#include <spdlog/spdlog.h>

namespace polymarket {

using json = nlohmann::json;

BinanceFeed::BinanceFeed(const std::string& proxy_url)
    : http_(4, proxy_url) {}

static int64_t now_ms_local() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

double BinanceFeed::fetch_price(const std::string& binance_symbol) {
    std::string url = std::string(BINANCE_API) + "/api/v3/ticker/price?symbol=" + binance_symbol;
    auto resp = http_.get(url);
    if (resp.status_code != 200) {
        throw std::runtime_error("Binance price fetch failed: status=" +
                                 std::to_string(resp.status_code));
    }
    auto j = json::parse(resp.body);
    return std::stod(j["price"].get<std::string>());
}

std::vector<BtcCandle> BinanceFeed::fetch_klines(const std::string& binance_symbol,
                                                 const std::string& interval,
                                                 int limit) {
    std::string url = std::string(BINANCE_API) +
                      "/api/v3/klines?symbol=" + binance_symbol + "&interval=" + interval + "&limit=" +
                      std::to_string(limit);
    auto resp = http_.get(url);
    if (resp.status_code != 200) {
        throw std::runtime_error("Binance klines fetch failed: status=" +
                                 std::to_string(resp.status_code));
    }

    auto j = json::parse(resp.body);
    std::vector<BtcCandle> candles;
    for (const auto& k : j) {
        BtcCandle c;
        c.open_time  = k[0].get<int64_t>();
        c.open       = std::stod(k[1].get<std::string>());
        c.high       = std::stod(k[2].get<std::string>());
        c.low        = std::stod(k[3].get<std::string>());
        c.close      = std::stod(k[4].get<std::string>());
        c.volume     = std::stod(k[5].get<std::string>());
        c.close_time = k[6].get<int64_t>();
        candles.push_back(c);
    }
    return candles;
}

BtcMarketData BinanceFeed::compute(const std::string& coin, double price, const std::vector<BtcCandle>& klines) {
    BtcMarketData data;
    data.symbol = coin;
    data.current_price = price;

    if (klines.empty()) return data;

    // 最后一根 K 线是当前正在进行的
    const auto& current = klines.back();
    data.strike_price = current.open;
    data.candle_open_time = current.open_time;
    data.candle_close_time = current.close_time;

    // 当前 1h 波动率: (high - low) / open
    if (current.open > 0) {
        data.current_1h_vol = (current.high - current.low) / current.open;
    }

    // 已过去和剩余分钟
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    data.minutes_into_candle = static_cast<int>((now_ms - current.open_time) / 60000);
    data.minutes_remaining = static_cast<int>((current.close_time - now_ms) / 60000);

    // 价格方向
    data.price_above_strike = (price >= data.strike_price);
    if (data.strike_price > 0) {
        data.deviation_pct = (price - data.strike_price) / data.strike_price * 100.0;
    }

    // 近 24 小时平均 1h 波动率（排除当前 K 线）
    if (klines.size() > 1) {
        double sum_vol = 0;
        int count = 0;
        // 倒数第2根到最多往前24根
        size_t start = (klines.size() > 25) ? klines.size() - 25 : 0;
        for (size_t i = start; i < klines.size() - 1; i++) {
            if (klines[i].open > 0) {
                sum_vol += (klines[i].high - klines[i].low) / klines[i].open;
                count++;
            }
        }
        if (count > 0) data.avg_24h_vol = sum_vol / count;
    }

    return data;
}

BtcMarketData BinanceFeed::fetch() {
    return fetch("BTC", "BTCUSDT");
}

BtcMarketData BinanceFeed::fetch(const std::string& coin, const std::string& binance_symbol) {
    auto price = fetch_price(binance_symbol);
    auto now = now_ms_local();
    auto& cache = kline_cache_[binance_symbol];
    bool need_klines = cache.klines.empty() || (now - cache.fetched_at_ms) >= 60000;
    if (need_klines) {
        try {
            cache.klines = fetch_klines(binance_symbol, "1h", 25);
            cache.fetched_at_ms = now;
        } catch (const std::exception& e) {
            if (cache.klines.empty()) throw;
            spdlog::warn("{} klines refresh failed, using cached candles: {}", coin, e.what());
        }
    }
    auto klines = cache.klines;
    latest_ = compute(coin, price, klines);

    spdlog::info("{}: ${:.4f} | strike: ${:.4f} | dev: {:+.2f}% | vol: {:.4f} (avg: {:.4f}) | {}min left",
                 coin, latest_.current_price, latest_.strike_price,
                 latest_.deviation_pct, latest_.current_1h_vol,
                 latest_.avg_24h_vol, latest_.minutes_remaining);

    return latest_;
}

BtcMarketData BinanceFeed::fetch_period(const std::string& coin,
                                         const std::string& binance_symbol,
                                         const std::string& interval,
                                         int period_minutes) {
    auto price = fetch_price(binance_symbol);
    auto cache_key = binance_symbol + ":" + interval;
    auto now = now_ms_local();
    auto& cache = kline_cache_[cache_key];
    bool need_klines = cache.klines.empty() || (now - cache.fetched_at_ms) >= 60000;
    if (need_klines) {
        try {
            cache.klines = fetch_klines(binance_symbol, interval, 25);
            cache.fetched_at_ms = now;
        } catch (const std::exception& e) {
            if (cache.klines.empty()) throw;
            spdlog::warn("{} {} klines refresh failed, using cached candles: {}",
                         coin, interval, e.what());
        }
    }
    auto data = compute(coin, price, cache.klines);
    if (data.candle_open_time > 0) {
        data.minutes_into_candle = static_cast<int>((now - data.candle_open_time) / 60000);
        data.minutes_remaining = std::max(0, period_minutes - data.minutes_into_candle);
    }
    spdlog::info("{} {}: ${:.4f} | strike: ${:.4f} | dev: {:+.2f}% | {}min left",
                 coin, interval, data.current_price, data.strike_price,
                 data.deviation_pct, data.minutes_remaining);
    return data;
}

}  // namespace polymarket
