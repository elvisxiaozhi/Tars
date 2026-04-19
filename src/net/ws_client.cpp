#include "net/ws_client.h"

#include <atomic>
#include <cstdlib>
#include <stdexcept>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <spdlog/spdlog.h>

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace asio = boost::asio;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;

namespace polymarket::net {

struct ParsedWsUrl {
    bool secure = false;
    std::string host;
    std::string port;
    std::string path;
};

struct ProxyInfo {
    bool enabled = false;
    std::string host;
    std::string port;
};

static ParsedWsUrl parse_ws_url(const std::string& url) {
    ParsedWsUrl result;

    std::string rest;
    if (url.rfind("wss://", 0) == 0) {
        result.secure = true;
        rest = url.substr(6);
    } else if (url.rfind("ws://", 0) == 0) {
        result.secure = false;
        rest = url.substr(5);
    } else {
        throw std::invalid_argument("URL must start with ws:// or wss://");
    }

    auto slash_pos = rest.find('/');
    std::string host_port;
    if (slash_pos == std::string::npos) {
        host_port = rest;
        result.path = "/";
    } else {
        host_port = rest.substr(0, slash_pos);
        result.path = rest.substr(slash_pos);
    }

    auto colon_pos = host_port.find(':');
    if (colon_pos == std::string::npos) {
        result.host = host_port;
        result.port = result.secure ? "443" : "80";
    } else {
        result.host = host_port.substr(0, colon_pos);
        result.port = host_port.substr(colon_pos + 1);
    }

    return result;
}

static ProxyInfo detect_proxy(const std::string& config_proxy = "") {
    ProxyInfo info;
    std::string proxy_url;
    if (!config_proxy.empty()) {
        proxy_url = config_proxy;
    } else {
        const char* proxy_env = std::getenv("HTTPS_PROXY");
        if (!proxy_env) proxy_env = std::getenv("https_proxy");
        if (!proxy_env) proxy_env = std::getenv("HTTP_PROXY");
        if (!proxy_env) proxy_env = std::getenv("http_proxy");
        if (!proxy_env) return info;
        proxy_url = proxy_env;
    }
    std::string rest;
    if (proxy_url.rfind("http://", 0) == 0)
        rest = proxy_url.substr(7);
    else if (proxy_url.rfind("https://", 0) == 0)
        rest = proxy_url.substr(8);
    else
        rest = proxy_url;

    if (!rest.empty() && rest.back() == '/') rest.pop_back();

    auto colon = rest.find(':');
    if (colon == std::string::npos) {
        info.host = rest;
        info.port = "8080";
    } else {
        info.host = rest.substr(0, colon);
        info.port = rest.substr(colon + 1);
    }
    info.enabled = true;
    return info;
}

using WssStream = websocket::stream<beast::ssl_stream<beast::tcp_stream>>;
using WsStream = websocket::stream<beast::tcp_stream>;

struct WsClient::Impl {
    asio::io_context ioc;
    ssl::context ssl_ctx{ssl::context::tlsv12_client};
    int timeout_sec;
    ProxyInfo proxy;
    std::atomic<bool> connected{false};
    std::atomic<bool> closing{false};

    // Only one of these is active at a time
    std::unique_ptr<WssStream> wss_stream;
    std::unique_ptr<WsStream> ws_stream;
    bool is_secure = false;

    WsMessageCallback on_msg;
    WsErrorCallback on_err;
    WsConnectCallback on_conn;
    WsCloseCallback on_cls;

    explicit Impl(int timeout, const std::string& proxy_url = "")
        : timeout_sec(timeout) {
        ssl_ctx.set_default_verify_paths();
        ssl_ctx.set_verify_mode(ssl::verify_peer);
        proxy = detect_proxy(proxy_url);
    }

    void connect_via_proxy(beast::tcp_stream& tcp_stream,
                           const ParsedWsUrl& parsed) {
        tcp::resolver resolver(ioc);
        auto proxy_results = resolver.resolve(proxy.host, proxy.port);

        tcp_stream.expires_after(std::chrono::seconds(timeout_sec));
        tcp_stream.connect(proxy_results);

        std::string connect_host = parsed.host + ":" + parsed.port;
        http::request<http::empty_body> connect_req{
            http::verb::connect, connect_host, 11};
        connect_req.set(http::field::host, connect_host);

        tcp_stream.expires_after(std::chrono::seconds(timeout_sec));
        http::write(tcp_stream, connect_req);

        beast::flat_buffer buf;
        http::response<http::empty_body> connect_res;
        http::response_parser<http::empty_body> parser(connect_res);
        parser.skip(true);

        tcp_stream.expires_after(std::chrono::seconds(timeout_sec));
        http::read(tcp_stream, buf, parser);

        if (connect_res.result() != http::status::ok) {
            throw std::runtime_error(
                "Proxy CONNECT failed: " +
                std::to_string(connect_res.result_int()));
        }
        spdlog::debug("WS proxy CONNECT tunnel established");
    }

    void connect(const std::string& url) {
        auto parsed = parse_ws_url(url);
        is_secure = parsed.secure;

        spdlog::debug("WS connecting to {}", url);

        if (is_secure) {
            connect_wss(parsed);
        } else {
            connect_ws(parsed);
        }

        connected = true;
        if (on_conn) on_conn();
        spdlog::info("WS connected to {}", url);
    }

    void connect_wss(const ParsedWsUrl& parsed) {
        wss_stream = std::make_unique<WssStream>(ioc, ssl_ctx);

        auto& tcp_layer = beast::get_lowest_layer(*wss_stream);

        // SNI
        if (!SSL_set_tlsext_host_name(
                wss_stream->next_layer().native_handle(),
                parsed.host.c_str())) {
            throw std::runtime_error("Failed to set SNI hostname");
        }

        if (proxy.enabled) {
            connect_via_proxy(tcp_layer, parsed);
        } else {
            tcp::resolver resolver(ioc);
            auto results = resolver.resolve(parsed.host, parsed.port);
            tcp_layer.expires_after(std::chrono::seconds(timeout_sec));
            tcp_layer.connect(results);
        }

        // TLS handshake
        tcp_layer.expires_after(std::chrono::seconds(timeout_sec));
        wss_stream->next_layer().handshake(ssl::stream_base::client);

        // Disable timeout for websocket (beast manages its own)
        tcp_layer.expires_never();

        // WebSocket handshake
        wss_stream->set_option(websocket::stream_base::decorator(
            [&parsed](websocket::request_type& req) {
                req.set(http::field::host, parsed.host);
                req.set(http::field::user_agent, "polymarket-arb/0.1");
            }));

        wss_stream->handshake(parsed.host, parsed.path);
    }

    void connect_ws(const ParsedWsUrl& parsed) {
        ws_stream = std::make_unique<WsStream>(ioc);

        auto& tcp_layer = beast::get_lowest_layer(*ws_stream);

        if (proxy.enabled) {
            connect_via_proxy(tcp_layer, parsed);
        } else {
            tcp::resolver resolver(ioc);
            auto results = resolver.resolve(parsed.host, parsed.port);
            tcp_layer.expires_after(std::chrono::seconds(timeout_sec));
            tcp_layer.connect(results);
        }

        tcp_layer.expires_never();

        ws_stream->set_option(websocket::stream_base::decorator(
            [&parsed](websocket::request_type& req) {
                req.set(http::field::host, parsed.host);
                req.set(http::field::user_agent, "polymarket-arb/0.1");
            }));

        ws_stream->handshake(parsed.host, parsed.path);
    }

    void send(const std::string& msg) {
        if (!connected) throw std::runtime_error("WebSocket not connected");

        if (is_secure) {
            wss_stream->write(asio::buffer(msg));
        } else {
            ws_stream->write(asio::buffer(msg));
        }
    }

    void run() {
        beast::flat_buffer buffer;

        while (connected && !closing) {
            try {
                buffer.clear();
                if (is_secure) {
                    wss_stream->read(buffer);
                } else {
                    ws_stream->read(buffer);
                }

                auto msg = beast::buffers_to_string(buffer.data());
                if (on_msg) on_msg(msg);

            } catch (const beast::system_error& se) {
                if (se.code() == websocket::error::closed) {
                    spdlog::info("WS connection closed normally");
                    break;
                }
                if (closing) break;  // Expected during shutdown
                spdlog::error("WS read error: {}", se.what());
                if (on_err) on_err(se.what());
                break;
            }
        }

        connected = false;
        if (on_cls) on_cls();
    }

    void close() {
        closing = true;
        if (!connected) return;

        try {
            if (is_secure) {
                wss_stream->close(websocket::close_code::normal);
            } else {
                ws_stream->close(websocket::close_code::normal);
            }
        } catch (const std::exception& e) {
            spdlog::debug("WS close: {}", e.what());
        }
        connected = false;
    }
};

WsClient::WsClient(int timeout_sec, const std::string& proxy_url)
    : impl_(std::make_unique<Impl>(timeout_sec, proxy_url)) {}

WsClient::~WsClient() {
    if (impl_->connected) {
        impl_->close();
    }
}

void WsClient::on_message(WsMessageCallback cb) { impl_->on_msg = std::move(cb); }
void WsClient::on_error(WsErrorCallback cb) { impl_->on_err = std::move(cb); }
void WsClient::on_connect(WsConnectCallback cb) { impl_->on_conn = std::move(cb); }
void WsClient::on_close(WsCloseCallback cb) { impl_->on_cls = std::move(cb); }

void WsClient::connect(const std::string& url) { impl_->connect(url); }
void WsClient::send(const std::string& msg) { impl_->send(msg); }
void WsClient::run() { impl_->run(); }
void WsClient::close() { impl_->close(); }
bool WsClient::is_connected() const { return impl_->connected; }

}  // namespace polymarket::net
