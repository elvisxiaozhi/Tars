#include "net/api_server.h"

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <spdlog/spdlog.h>

namespace beast = boost::beast;
namespace http = beast::http;
namespace asio = boost::asio;
using tcp = asio::ip::tcp;

namespace polymarket::net {

struct ApiServer::Impl {
    asio::io_context ioc;
    ApiDataCallback status_cb;
    ApiDataCallback trades_cb;
    ApiDataCallback stats_cb;
    ApiDataCallback analytics_cb;
    ApiDataCallback experiment_status_cb;
    ApiDataCallback experiment_trades_cb;
    ApiDataCallback shutdown_cb;
    std::string dashboard_html;

    http::response<http::string_body>
    handle_request(const http::request<http::string_body>& req) {
        auto target = std::string(req.target());

        // 路由
        if (target == "/" || target == "/index.html") {
            return make_response(http::status::ok, dashboard_html, "text/html");
        }
        if (target == "/api/status" && status_cb) {
            return make_response(http::status::ok, status_cb(), "application/json");
        }
        if (target == "/api/trades" && trades_cb) {
            return make_response(http::status::ok, trades_cb(), "application/json");
        }
        if (target == "/api/stats" && stats_cb) {
            return make_response(http::status::ok, stats_cb(), "application/json");
        }
        if (target == "/api/analytics" && analytics_cb) {
            return make_response(http::status::ok, analytics_cb(), "application/json");
        }
        if (target == "/api/experiment/status" && experiment_status_cb) {
            return make_response(http::status::ok, experiment_status_cb(), "application/json");
        }
        if (target == "/api/experiment/trades" && experiment_trades_cb) {
            return make_response(http::status::ok, experiment_trades_cb(), "application/json");
        }
        if (target == "/api/shutdown" && shutdown_cb) {
            // 仅接受 POST/DELETE，避免 GET 误触发（如浏览器预取）
            if (req.method() != http::verb::post && req.method() != http::verb::delete_) {
                return make_response(http::status::method_not_allowed,
                                     R"({"error":"use POST"})", "application/json");
            }
            return make_response(http::status::ok, shutdown_cb(), "application/json");
        }

        return make_response(http::status::not_found, R"({"error":"not found"})", "application/json");
    }

    static http::response<http::string_body>
    make_response(http::status status, const std::string& body, const std::string& content_type) {
        http::response<http::string_body> res{status, 11};
        res.set(http::field::content_type, content_type + "; charset=utf-8");
        res.set(http::field::access_control_allow_origin, "*");
        res.set(http::field::cache_control, "no-cache");
        res.body() = body;
        res.prepare_payload();
        return res;
    }

    void handle_session(tcp::socket socket) {
        try {
            beast::flat_buffer buffer;
            http::request<http::string_body> req;
            http::read(socket, buffer, req);

            auto res = handle_request(req);
            http::write(socket, res);

            beast::error_code ec;
            socket.shutdown(tcp::socket::shutdown_send, ec);
        } catch (const std::exception& e) {
            spdlog::debug("API session error: {}", e.what());
        }
    }
};

ApiServer::ApiServer(int port) : impl_(std::make_unique<Impl>()), port_(port) {}

ApiServer::~ApiServer() { stop(); }

void ApiServer::on_status(ApiDataCallback cb) { impl_->status_cb = std::move(cb); }
void ApiServer::on_trades(ApiDataCallback cb) { impl_->trades_cb = std::move(cb); }
void ApiServer::on_stats(ApiDataCallback cb) { impl_->stats_cb = std::move(cb); }
void ApiServer::on_analytics(ApiDataCallback cb) { impl_->analytics_cb = std::move(cb); }
void ApiServer::on_experiment_status(ApiDataCallback cb) { impl_->experiment_status_cb = std::move(cb); }
void ApiServer::on_experiment_trades(ApiDataCallback cb) { impl_->experiment_trades_cb = std::move(cb); }
void ApiServer::on_shutdown(ApiDataCallback cb) { impl_->shutdown_cb = std::move(cb); }
void ApiServer::set_dashboard_html(const std::string& html) { impl_->dashboard_html = html; }

void ApiServer::start() {
    if (running_) return;
    running_ = true;

    server_thread_ = std::thread([this]() {
        try {
            tcp::acceptor acceptor(impl_->ioc,
                tcp::endpoint(tcp::v4(), static_cast<unsigned short>(port_)));
            acceptor.set_option(asio::socket_base::reuse_address(true));

            spdlog::info("API server listening on http://localhost:{}", port_);

            while (running_) {
                // 设置非阻塞等待，每秒检查 running_ 状态
                acceptor.non_blocking(true);
                beast::error_code ec;

                tcp::socket socket(impl_->ioc);
                acceptor.accept(socket, ec);

                if (ec == asio::error::would_block) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    continue;
                }
                if (ec) {
                    if (running_) spdlog::debug("Accept error: {}", ec.message());
                    continue;
                }

                // 处理请求（同步，简单够用）
                impl_->handle_session(std::move(socket));
            }
        } catch (const std::exception& e) {
            spdlog::error("API server error: {}", e.what());
            running_ = false;
        }
    });
}

void ApiServer::stop() {
    running_ = false;
    if (server_thread_.joinable()) {
        server_thread_.join();
    }
}

}  // namespace polymarket::net
