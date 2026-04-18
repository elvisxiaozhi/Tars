#include <chrono>
#include <thread>

#include <boost/version.hpp>
#include <openssl/opensslv.h>
#include <sqlite3.h>
#include <secp256k1.h>
#include <spdlog/spdlog.h>
#include <json.hpp>

#include "net/http_client.h"
#include "net/ws_client.h"
#include "utils/types.h"
#include "utils/json_helpers.h"

namespace jh = polymarket::json_helpers;
using json = nlohmann::json;

int main() {
    spdlog::info("polymarket-arb v0.1.0");
    spdlog::info("boost     {}.{}.{}",
                 BOOST_VERSION / 100000,
                 BOOST_VERSION / 100 % 1000,
                 BOOST_VERSION % 100);
    spdlog::info("openssl   {}", OPENSSL_VERSION_TEXT);
    spdlog::info("sqlite    {}", sqlite3_libversion());
    spdlog::info("nlohmann  {}.{}.{}",
                 NLOHMANN_JSON_VERSION_MAJOR,
                 NLOHMANN_JSON_VERSION_MINOR,
                 NLOHMANN_JSON_VERSION_PATCH);

    secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
    if (ctx) {
        spdlog::info("secp256k1 OK");
        secp256k1_context_destroy(ctx);
    }

    spdlog::info("all dependencies verified");

    polymarket::net::HttpClient http(10);

    // --- 测试 1: 解析市场列表 ---
    spdlog::info("--- JSON parse test: markets ---");
    try {
        auto resp = http.get("https://clob.polymarket.com/sampling-markets?limit=3");
        auto j = json::parse(resp.body);
        auto markets = jh::parse_markets_response(j);
        spdlog::info("Parsed {} markets", markets.size());
        for (const auto& m : markets) {
            spdlog::info("  [{}] {} (tokens: {}, tick: {})",
                         m.active ? "active" : "closed",
                         m.question.substr(0, 50),
                         m.tokens.size(),
                         m.minimum_tick_size);
            for (const auto& t : m.tokens) {
                spdlog::info("    {} ${:.3f} ({})",
                             t.outcome, t.price, t.token_id.substr(0, 20));
            }
        }
    } catch (const std::exception& e) {
        spdlog::error("Markets parse failed: {}", e.what());
    }

    // --- 测试 2: 解析订单簿 ---
    spdlog::info("--- JSON parse test: order book ---");
    try {
        auto resp = http.get(
            "https://clob.polymarket.com/book?"
            "token_id=69422147515888934539342749343952767069189843320299332157580167457035825622340");
        auto j = json::parse(resp.body);
        auto ob = jh::parse_order_book(j);
        auto bba = jh::extract_best_bid_ask(ob);

        spdlog::info("OrderBook: {} bids, {} asks", ob.bids.size(), ob.asks.size());
        spdlog::info("Best bid: ${:.2f} x {:.2f}, best ask: ${:.2f} x {:.2f}",
                     bba.best_bid, bba.bid_size, bba.best_ask, bba.ask_size);
    } catch (const std::exception& e) {
        spdlog::error("OrderBook parse failed: {}", e.what());
    }

    // --- 测试 3: WebSocket 事件解析 ---
    spdlog::info("--- JSON parse test: WebSocket events ---");
    polymarket::net::WsClient ws(10);
    int msg_count = 0;

    ws.on_message([&](const std::string& msg) {
        msg_count++;
        try {
            auto j = json::parse(msg);

            if (j.is_array()) {
                // book snapshot (array of order books)
                for (const auto& item : j) {
                    auto ob = jh::parse_order_book(item);
                    auto bba = jh::extract_best_bid_ask(ob);
                    spdlog::info("WS [book] asset={}... bid=${:.2f} ask=${:.2f}",
                                 ob.asset_id.substr(0, 15), bba.best_bid, bba.best_ask);
                }
            } else if (j.contains("price_changes")) {
                // price_change event
                for (const auto& pc_json : j["price_changes"]) {
                    auto pc = jh::parse_ws_price_change(pc_json);
                    spdlog::info("WS [price] asset={}... ${:.2f} x {:.2f} {}",
                                 pc.asset_id.substr(0, 15), pc.price, pc.size, pc.side);
                }
            } else if (j.contains("last_trade_price")) {
                auto lt = jh::parse_ws_last_trade(j["last_trade_price"]);
                spdlog::info("WS [trade] asset={}... ${:.2f} x {:.2f}",
                             lt.asset_id.substr(0, 15), lt.price, lt.size);
            }
        } catch (const std::exception& e) {
            spdlog::warn("WS parse failed: {} | raw: {}", e.what(), msg.substr(0, 100));
        }
    });

    ws.on_error([](const std::string& err) {
        spdlog::error("WS error: {}", err);
    });

    try {
        ws.connect("wss://ws-subscriptions-clob.polymarket.com/ws/market");

        json sub_msg = {
            {"type", "market"},
            {"assets_ids", json::array({
                "110577858780148651266447864913251141042726823916797235908223754289720625159449",
                "72231738155593996978526441927387175895154152018539080152695312064797491581743"
            })}
        };
        ws.send(sub_msg.dump());
        spdlog::info("WS subscribed");

        std::thread ws_thread([&ws] { ws.run(); });
        std::this_thread::sleep_for(std::chrono::seconds(10));

        spdlog::info("WS received {} messages in 10s", msg_count);
        ws.close();
        if (ws_thread.joinable()) ws_thread.join();
    } catch (const std::exception& e) {
        spdlog::error("WS test failed: {}", e.what());
    }

    spdlog::info("done");
    return 0;
}
