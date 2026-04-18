#include <chrono>
#include <thread>

#include <spdlog/spdlog.h>
#include <json.hpp>

#include "net/http_client.h"
#include "net/ws_client.h"
#include "utils/types.h"
#include "utils/json_helpers.h"
#include "utils/config.h"

namespace jh = polymarket::json_helpers;
using json = nlohmann::json;

int main(int argc, char* argv[]) {
    // 配置文件路径（默认 config/config.json）
    std::string config_path = "config/config.json";
    if (argc > 1) config_path = argv[1];

    // 加载配置
    polymarket::AppConfig cfg;
    try {
        cfg = polymarket::load_config(config_path);
    } catch (const std::exception& e) {
        spdlog::error("Failed to load config: {}", e.what());
        return 1;
    }

    // 初始化日志
    polymarket::init_logging(cfg.logging);

    spdlog::info("polymarket-arb v0.1.0");
    spdlog::info("config loaded from: {}", config_path);
    spdlog::info("CLOB REST: {}", cfg.polymarket.clob_rest_url);
    spdlog::info("CLOB WS:   {}", cfg.polymarket.clob_ws_url);
    spdlog::info("min profit: {}%, max trade: ${:.0f}",
                 cfg.strategy.min_net_profit_pct,
                 cfg.strategy.max_trade_size_usdc);
    spdlog::info("risk: daily_loss_limit=${:.0f}, kill_switch={}",
                 cfg.risk.daily_loss_limit,
                 cfg.risk.kill_switch ? "ON" : "OFF");

    // 快速验证: 请求 CLOB 时间
    polymarket::net::HttpClient http(10);
    try {
        auto resp = http.get(cfg.polymarket.clob_rest_url + "/time");
        spdlog::info("CLOB /time: {} (status={})", resp.body, resp.status_code);
    } catch (const std::exception& e) {
        spdlog::error("CLOB connection failed: {}", e.what());
        return 1;
    }

    spdlog::info("ready");
    return 0;
}
