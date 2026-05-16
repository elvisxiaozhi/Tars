#pragma once

#include <map>
#include <string>

#include "core/binance_feed.h"
#include "net/http_client.h"
#include "utils/types.h"

namespace polymarket {

class FinanceFeed {
public:
    explicit FinanceFeed(const std::string& proxy_url = "");

    static std::string identify_asset(const Market& market);

    BtcMarketData fetch_for_market(const Market& market);

private:
    struct AssetQuote {
        double current = 0;
        double session_open = 0;
        int64_t session_close_ms = 0;
        int64_t fetched_at_ms = 0;
    };

    AssetQuote fetch_asset(const std::string& asset);
    double threshold_for_market(const Market& market, const std::string& asset) const;

    net::HttpClient http_;
    std::map<std::string, AssetQuote> cache_;
};

}  // namespace polymarket
