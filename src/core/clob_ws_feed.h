#pragma once

#include <atomic>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include <json.hpp>

#include "core/quote_cache.h"
#include "net/ws_client.h"

namespace polymarket {

class ClobWsFeed {
public:
    ClobWsFeed(std::string ws_url, std::string proxy_url, QuoteCache& cache);
    ~ClobWsFeed();

    ClobWsFeed(const ClobWsFeed&) = delete;
    ClobWsFeed& operator=(const ClobWsFeed&) = delete;

    void start();
    void stop();
    void update_subscriptions(const std::set<std::string>& token_ids);

    bool connected() const { return connected_; }
    size_t subscribed_count() const;

private:
    void run_loop();
    void send_subscription_locked(const std::set<std::string>& token_ids,
                                  const std::string& operation);
    void send_text_locked(const std::string& msg);
    void handle_message(const std::string& msg);
    void handle_json_message(const nlohmann::json& j);

    std::string ws_url_;
    std::string proxy_url_;
    QuoteCache& cache_;

    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::thread worker_;

    mutable std::mutex mu_;
    std::mutex send_mu_;
    std::set<std::string> desired_assets_;
    std::set<std::string> subscribed_assets_;
    net::WsClient* active_client_ = nullptr;
};

}  // namespace polymarket
