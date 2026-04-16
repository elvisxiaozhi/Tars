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

    // HTTP 客户端测试
    spdlog::info("--- HTTP client test ---");
    polymarket::net::HttpClient http(10);
    try {
        auto resp = http.get("https://clob.polymarket.com/time");
        spdlog::info("GET /time status={}", resp.status_code);
        spdlog::info("body: {}", resp.body);
    } catch (const std::exception& e) {
        spdlog::error("HTTP request failed: {}", e.what());
    }

    // WebSocket 客户端测试
    spdlog::info("--- WebSocket client test ---");
    polymarket::net::WsClient ws(10);

    int msg_count = 0;

    ws.on_connect([] {
        spdlog::info("WS on_connect fired");
    });

    ws.on_message([&](const std::string& msg) {
        msg_count++;
        // 只打印前 200 字符避免刷屏
        auto preview = msg.substr(0, 200);
        spdlog::info("WS msg #{}: {}{}",
                     msg_count, preview, msg.size() > 200 ? "..." : "");
    });

    ws.on_error([](const std::string& err) {
        spdlog::error("WS error: {}", err);
    });

    ws.on_close([] {
        spdlog::info("WS on_close fired");
    });

    try {
        ws.connect("wss://ws-subscriptions-clob.polymarket.com/ws/market");

        // Polymarket WS 订阅格式:
        // {"assets_ids": ["token_id_yes", "token_id_no"], "type": "market"}
        // 订阅高流动性市场
        nlohmann::json sub_msg = {
            {"type", "market"},
            {"assets_ids", nlohmann::json::array({
                "110577858780148651266447864913251141042726823916797235908223754289720625159449",
                "72231738155593996978526441927387175895154152018539080152695312064797491581743"
            })}
        };
        ws.send(sub_msg.dump());
        spdlog::info("WS subscribed to active market");

        // 在单独线程运行消息循环，主线程等几秒后关闭
        std::thread ws_thread([&ws] { ws.run(); });

        std::this_thread::sleep_for(std::chrono::seconds(10));

        spdlog::info("WS received {} messages in 10s, closing...", msg_count);
        ws.close();

        if (ws_thread.joinable()) ws_thread.join();
    } catch (const std::exception& e) {
        spdlog::error("WS test failed: {}", e.what());
    }

    spdlog::info("done");
    return 0;
}
