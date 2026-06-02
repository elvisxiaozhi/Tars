#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace polymarket::net {

// API 数据回调：由 main 注册，server 调用获取最新数据
using ApiDataCallback = std::function<std::string()>;

class ApiServer {
public:
    explicit ApiServer(int port = 8080, std::string host = "127.0.0.1");
    ~ApiServer();

    ApiServer(const ApiServer&) = delete;
    ApiServer& operator=(const ApiServer&) = delete;

    // 注册数据回调
    void on_status(ApiDataCallback cb);
    void on_trades(ApiDataCallback cb);
    void on_stats(ApiDataCallback cb);
    void on_analytics(ApiDataCallback cb);
    void on_finance_experiment_status(ApiDataCallback cb);
    void on_finance_experiment_trades(ApiDataCallback cb);
    void on_finance_experiment_all_trades(ApiDataCallback cb);
    void on_crypto_4h_experiment_status(ApiDataCallback cb);
    void on_crypto_4h_experiment_trades(ApiDataCallback cb);
    void on_crypto_4h_experiment_all_trades(ApiDataCallback cb);
    void on_crypto_daily_experiment_status(ApiDataCallback cb);
    void on_crypto_daily_experiment_trades(ApiDataCallback cb);
    void on_crypto_daily_experiment_all_trades(ApiDataCallback cb);
    void on_trend_v2_experiment_status(ApiDataCallback cb);
    void on_trend_v2_experiment_trades(ApiDataCallback cb);
    void on_trend_v2_experiment_all_trades(ApiDataCallback cb);
    void on_trend_v3_experiment_status(ApiDataCallback cb);
    void on_trend_v3_experiment_trades(ApiDataCallback cb);
    void on_trend_v3_experiment_all_trades(ApiDataCallback cb);
    // POST /api/shutdown — 触发优雅退出（设 g_running=false → 主循环退出 → emergency_close_all）
    void on_shutdown(ApiDataCallback cb);

    // 设置前端 HTML 内容（嵌入到二进制中）
    void set_dashboard_html(const std::string& html);

    // 在独立线程中启动
    void start();
    void stop();

    bool is_running() const { return running_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<bool> running_{false};
    std::thread server_thread_;
    int port_;
    std::string host_;
};

}  // namespace polymarket::net
