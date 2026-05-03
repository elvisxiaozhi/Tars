#include "core/clob_ws_feed.h"

#include <algorithm>
#include <chrono>
#include <utility>
#include <vector>

#include <json.hpp>
#include <spdlog/spdlog.h>

#include "utils/json_helpers.h"

namespace polymarket {

using json = nlohmann::json;
namespace jh = json_helpers;

static int64_t now_ms_ws() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

ClobWsFeed::ClobWsFeed(std::string ws_url, std::string proxy_url, QuoteCache& cache)
    : ws_url_(std::move(ws_url)), proxy_url_(std::move(proxy_url)), cache_(cache) {}

ClobWsFeed::~ClobWsFeed() {
    stop();
}

void ClobWsFeed::start() {
    if (running_) return;
    running_ = true;
    worker_ = std::thread(&ClobWsFeed::run_loop, this);
}

void ClobWsFeed::stop() {
    running_ = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (active_client_) active_client_->close();
    }
    if (worker_.joinable()) worker_.join();
}

size_t ClobWsFeed::subscribed_count() const {
    std::lock_guard<std::mutex> lock(mu_);
    return subscribed_assets_.size();
}

void ClobWsFeed::update_subscriptions(const std::set<std::string>& token_ids) {
    std::set<std::string> to_add;
    std::set<std::string> to_remove;
    {
        std::lock_guard<std::mutex> lock(mu_);
        desired_assets_ = token_ids;
        for (const auto& id : desired_assets_) {
            if (subscribed_assets_.find(id) == subscribed_assets_.end()) {
                to_add.insert(id);
            }
        }
        for (const auto& id : subscribed_assets_) {
            if (desired_assets_.find(id) == desired_assets_.end()) {
                to_remove.insert(id);
            }
        }
        if (active_client_ && connected_) {
            try {
                if (!to_add.empty()) {
                    send_subscription_locked(to_add, "subscribe");
                    subscribed_assets_.insert(to_add.begin(), to_add.end());
                }
                if (!to_remove.empty()) {
                    send_subscription_locked(to_remove, "unsubscribe");
                    for (const auto& id : to_remove) subscribed_assets_.erase(id);
                }
                if (desired_assets_.empty()) {
                    active_client_->close();
                }
            } catch (const std::exception& e) {
                spdlog::warn("CLOB WS subscription update failed: {}", e.what());
                active_client_->close();
            }
        }
    }
}

void ClobWsFeed::run_loop() {
    int reconnect_delay_sec = 1;
    while (running_) {
        while (running_) {
            bool has_assets = false;
            {
                std::lock_guard<std::mutex> lock(mu_);
                has_assets = !desired_assets_.empty();
            }
            if (has_assets) break;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (!running_) break;

        net::WsClient client(8, proxy_url_);
        {
            std::lock_guard<std::mutex> lock(mu_);
            active_client_ = &client;
        }

        std::thread heartbeat;
        try {
            client.on_connect([&]() {
                connected_ = true;
                reconnect_delay_sec = 1;
                std::lock_guard<std::mutex> lock(mu_);
                subscribed_assets_.clear();
                if (!desired_assets_.empty()) {
                    send_subscription_locked(desired_assets_, "");
                    subscribed_assets_ = desired_assets_;
                    spdlog::info("CLOB WS subscribed {} asset(s)", subscribed_assets_.size());
                }
            });
            client.on_close([&]() {
                connected_ = false;
            });
            client.on_error([&](const std::string& err) {
                connected_ = false;
                spdlog::warn("CLOB WS error: {}", err);
            });
            client.on_message([&](const std::string& msg) {
                handle_message(msg);
            });

            client.connect(ws_url_);
            heartbeat = std::thread([&]() {
                while (running_ && client.is_connected()) {
                    for (int i = 0; i < 10 && running_ && client.is_connected(); ++i) {
                        std::this_thread::sleep_for(std::chrono::seconds(1));
                    }
                    if (!running_ || !client.is_connected()) break;
                    try {
                        std::lock_guard<std::mutex> send_lock(send_mu_);
                        client.send("PING");
                    } catch (const std::exception& e) {
                        spdlog::debug("CLOB WS heartbeat failed: {}", e.what());
                        break;
                    }
                }
            });
            client.run();
        } catch (const std::exception& e) {
            connected_ = false;
            if (running_) spdlog::warn("CLOB WS reconnect after error: {}", e.what());
        }

        if (heartbeat.joinable()) heartbeat.join();
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (active_client_ == &client) active_client_ = nullptr;
            subscribed_assets_.clear();
        }
        connected_ = false;

        if (!running_) break;
        std::this_thread::sleep_for(std::chrono::seconds(reconnect_delay_sec));
        reconnect_delay_sec = std::min(reconnect_delay_sec * 2, 30);
    }
}

void ClobWsFeed::send_subscription_locked(const std::set<std::string>& token_ids,
                                          const std::string& operation) {
    if (!active_client_ || token_ids.empty()) return;
    json sub;
    sub["assets_ids"] = std::vector<std::string>(token_ids.begin(), token_ids.end());
    sub["custom_feature_enabled"] = true;
    if (operation.empty()) {
        sub["type"] = "market";
    } else {
        sub["operation"] = operation;
    }
    send_text_locked(sub.dump());
}

void ClobWsFeed::send_text_locked(const std::string& msg) {
    std::lock_guard<std::mutex> send_lock(send_mu_);
    if (!active_client_) return;
    active_client_->send(msg);
}

void ClobWsFeed::handle_message(const std::string& msg) {
    if (msg == "PONG" || msg == "pong" || msg.empty()) return;
    try {
        auto j = json::parse(msg);
        handle_json_message(j);
    } catch (const std::exception& e) {
        spdlog::debug("CLOB WS parse ignored: {} msg={}", e.what(), msg);
    }
}

void ClobWsFeed::handle_json_message(const json& j) {
    if (j.is_array()) {
        for (const auto& item : j) handle_json_message(item);
        return;
    }
    if (!j.is_object()) return;

    const auto event_type = jh::get_string(j, "event_type");
    const int64_t ts = [&]() {
        auto t = jh::get_string(j, "timestamp");
        if (t.empty()) return now_ms_ws();
        try { return std::stoll(t); } catch (...) { return now_ms_ws(); }
    }();

    if (event_type == "book") {
        auto book = jh::parse_order_book(j);
        auto best = jh::extract_best_bid_ask(book);
        cache_.update_book(book, best, ts, true);
        return;
    }

    if (event_type == "best_bid_ask") {
        auto token_id = jh::get_string(j, "asset_id");
        cache_.update_best(token_id,
                           jh::get_double(j, "best_bid"),
                           jh::get_double(j, "best_ask"),
                           0.0, 0.0, ts, true);
        return;
    }

    if (event_type == "price_change" && j.contains("price_changes") &&
        j["price_changes"].is_array()) {
        for (const auto& pc : j["price_changes"]) {
            auto token_id = jh::get_string(pc, "asset_id");
            double best_bid = jh::get_double(pc, "best_bid");
            double best_ask = jh::get_double(pc, "best_ask");
            if (token_id.empty() || (best_bid <= 0 && best_ask <= 0)) continue;
            cache_.update_best(token_id, best_bid, best_ask, 0.0, 0.0, ts, true);
        }
    }
}

}  // namespace polymarket
