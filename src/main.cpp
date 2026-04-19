#include <filesystem>

#include <spdlog/spdlog.h>

#include "core/market_feed.h"
#include "utils/config.h"

// 从可执行文件位置向上查找 config/config.json
static std::string find_config(const char* argv0) {
    namespace fs = std::filesystem;
    // 1) 当前工作目录
    if (fs::exists("config/config.json")) return "config/config.json";
    // 2) 可执行文件所在目录逐级向上
    auto dir = fs::weakly_canonical(fs::path(argv0)).parent_path();
    for (int i = 0; i < 5; i++) {
        auto candidate = dir / "config" / "config.json";
        if (fs::exists(candidate)) return candidate.string();
        if (!dir.has_parent_path() || dir == dir.parent_path()) break;
        dir = dir.parent_path();
    }
    return "config/config.json";  // fallback, 让 load_config 报错
}

int main(int argc, char* argv[]) {
    std::string config_path = (argc > 1) ? argv[1] : find_config(argv[0]);

    polymarket::AppConfig cfg;
    try {
        cfg = polymarket::load_config(config_path);
    } catch (const std::exception& e) {
        spdlog::error("Failed to load config: {}", e.what());
        return 1;
    }

    polymarket::init_logging(cfg.logging);
    spdlog::info("polymarket-arb v0.1.0");

    // MarketFeed: 拉取市场列表 + 订单簿 + 打印报价
    polymarket::MarketFeed feed(cfg);

    feed.fetch_markets();
    if (feed.market_count() == 0) {
        spdlog::error("No active markets found");
        return 1;
    }

    feed.fetch_order_books();
    feed.print_summary();

    return 0;
}
