#include "core/finance_feed.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <regex>
#include <stdexcept>

#include <json.hpp>
#include <spdlog/spdlog.h>

namespace polymarket {

using json = nlohmann::json;

namespace {

int64_t now_ms_local() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string lower_copy(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string yahoo_symbol_for_asset(const std::string& asset) {
    if (asset == "SPX") return "%5EGSPC";
    if (asset == "SPY") return "SPY";
    if (asset == "GOLD") return "GC=F";
    if (asset == "SILVER") return "SI=F";
    if (asset == "WTI") return "CL=F";
    return "";
}

bool plausible_threshold(const std::string& asset, double value) {
    if (asset == "SPX") return value >= 1000 && value <= 10000;
    if (asset == "SPY") return value >= 100 && value <= 1000;
    if (asset == "GOLD") return value >= 1000 && value <= 10000;
    if (asset == "SILVER") return value >= 10 && value <= 200;
    if (asset == "WTI") return value >= 20 && value <= 300;
    return false;
}

double parse_number(std::string s) {
    s.erase(std::remove(s.begin(), s.end(), ','), s.end());
    return std::stod(s);
}

}  // namespace

FinanceFeed::FinanceFeed(const std::string& proxy_url)
    : http_(4, proxy_url) {}

std::string FinanceFeed::identify_asset(const Market& market) {
    std::string text = lower_copy(market.question + " " + market.market_slug);
    if (text.find("spy") != std::string::npos) return "SPY";
    if (text.find("spx") != std::string::npos ||
        text.find("s&p") != std::string::npos ||
        text.find("s-p-500") != std::string::npos) {
        return "SPX";
    }
    if (text.find("gold") != std::string::npos) return "GOLD";
    if (text.find("silver") != std::string::npos) return "SILVER";
    if (text.find("wti") != std::string::npos || text.find("oil") != std::string::npos) {
        return "WTI";
    }
    return "";
}

FinanceFeed::AssetQuote FinanceFeed::fetch_asset(const std::string& asset) {
    auto now = now_ms_local();
    auto cached = cache_.find(asset);
    if (cached != cache_.end() && now - cached->second.fetched_at_ms < 15000) {
        return cached->second;
    }

    std::string yahoo_symbol = yahoo_symbol_for_asset(asset);
    if (yahoo_symbol.empty()) {
        throw std::runtime_error("unsupported finance asset: " + asset);
    }

    std::string url = "https://query1.finance.yahoo.com/v8/finance/chart/" +
        yahoo_symbol + "?range=1d&interval=1m";
    auto resp = http_.get(url);
    if (resp.status_code != 200) {
        throw std::runtime_error("Yahoo chart fetch failed: status=" +
                                 std::to_string(resp.status_code));
    }

    auto root = json::parse(resp.body);
    const auto& result = root["chart"]["result"][0];
    const auto& meta = result["meta"];
    const auto& closes = result["indicators"]["quote"][0]["close"];

    AssetQuote quote;
    quote.current = meta.value("regularMarketPrice", 0.0);
    quote.session_open = 0.0;

    if (meta.contains("currentTradingPeriod") &&
        meta["currentTradingPeriod"].contains("regular")) {
        const auto& regular = meta["currentTradingPeriod"]["regular"];
        quote.session_close_ms = regular.value("end", 0LL) * 1000LL;
    }

    for (const auto& c : closes) {
        if (c.is_null()) continue;
        double v = c.get<double>();
        if (v > 0 && quote.session_open <= 0) quote.session_open = v;
        if (v > 0) quote.current = v;
    }
    if (quote.session_open <= 0) {
        quote.session_open = meta.value("previousClose", 0.0);
    }
    if (quote.current <= 0 || quote.session_open <= 0) {
        throw std::runtime_error("Yahoo chart missing usable price for " + asset);
    }
    quote.fetched_at_ms = now;
    cache_[asset] = quote;
    return quote;
}

double FinanceFeed::threshold_for_market(const Market& market, const std::string& asset) const {
    std::string text = market.question + " " + market.market_slug;
    std::regex num_re(R"(([0-9]{1,3}(?:,[0-9]{3})*(?:\.[0-9]+)?|[0-9]{2,5}(?:\.[0-9]+)?))");
    std::sregex_iterator it(text.begin(), text.end(), num_re);
    std::sregex_iterator end;
    for (; it != end; ++it) {
        double value = parse_number((*it)[1].str());
        if (plausible_threshold(asset, value)) return value;
    }
    return 0;
}

BtcMarketData FinanceFeed::fetch_for_market(const Market& market) {
    std::string asset = identify_asset(market);
    if (asset.empty()) {
        throw std::runtime_error("unsupported finance market: " + market.question);
    }
    auto quote = fetch_asset(asset);
    double threshold = threshold_for_market(market, asset);

    BtcMarketData data;
    data.symbol = asset;
    data.current_price = quote.current;
    data.strike_price = threshold > 0 ? threshold : quote.session_open;
    data.price_above_strike = data.current_price >= data.strike_price;
    if (data.strike_price > 0) {
        data.deviation_pct = (data.current_price - data.strike_price) /
            data.strike_price * 100.0;
    }
    data.candle_open_time = 0;
    data.candle_close_time = quote.session_close_ms;
    auto now = now_ms_local();
    data.minutes_remaining = quote.session_close_ms > now
        ? static_cast<int>((quote.session_close_ms - now) / 60000)
        : 0;
    data.minutes_into_candle = data.minutes_remaining > 0
        ? std::max(0, 390 - data.minutes_remaining)
        : 0;
    if (quote.session_open > 0) {
        data.current_1h_vol = std::abs(data.current_price - quote.session_open) /
            quote.session_open;
    }
    data.avg_24h_vol = data.current_1h_vol;

    spdlog::info("FIN {}: {:.4f} | ref: {:.4f} | dev: {:+.2f}% | {}min left | {}",
                 asset, data.current_price, data.strike_price, data.deviation_pct,
                 data.minutes_remaining, market.question);
    return data;
}

}  // namespace polymarket
