#include "net/http_client.h"

#include <chrono>
#include <cstdlib>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <spdlog/spdlog.h>

namespace beast = boost::beast;
namespace http = beast::http;
namespace asio = boost::asio;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;

namespace polymarket::net {

struct ParsedUrl {
    bool https = false;
    std::string host;
    std::string port;
    std::string target;
};

struct ProxyInfo {
    bool enabled = false;
    std::string host;
    std::string port;
};

static ParsedUrl parse_url(const std::string& url) {
    ParsedUrl result;

    std::string rest;
    if (url.rfind("https://", 0) == 0) {
        result.https = true;
        rest = url.substr(8);
    } else if (url.rfind("http://", 0) == 0) {
        result.https = false;
        rest = url.substr(7);
    } else {
        throw std::invalid_argument("URL must start with http:// or https://");
    }

    auto slash_pos = rest.find('/');
    std::string host_port;
    if (slash_pos == std::string::npos) {
        host_port = rest;
        result.target = "/";
    } else {
        host_port = rest.substr(0, slash_pos);
        result.target = rest.substr(slash_pos);
    }

    auto colon_pos = host_port.find(':');
    if (colon_pos == std::string::npos) {
        result.host = host_port;
        result.port = result.https ? "443" : "80";
    } else {
        result.host = host_port.substr(0, colon_pos);
        result.port = host_port.substr(colon_pos + 1);
    }

    return result;
}

static ProxyInfo detect_proxy(const std::string& config_proxy = "") {
    ProxyInfo info;
    // Config proxy takes priority over env vars
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
    // Strip scheme
    std::string rest;
    if (proxy_url.rfind("http://", 0) == 0) {
        rest = proxy_url.substr(7);
    } else if (proxy_url.rfind("https://", 0) == 0) {
        rest = proxy_url.substr(8);
    } else {
        rest = proxy_url;
    }
    // Remove trailing slash
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
    spdlog::debug("Using proxy {}:{}", info.host, info.port);
    return info;
}

struct HttpClient::Impl {
    // ssl_ctx 保留为成员（OpenSSL 内部线程安全，重复使用避免每次重新加载证书）。
    // io_context 改为每次 request 栈上局部对象——这是 wall-clock guard 能 detach
    // 卡死线程而不破坏后续调用的前提（共享 ioc 在多线程同步使用时未定义）。
    ssl::context ssl_ctx{ssl::context::tls_client};  // 允许 TLS 1.2/1.3 协商
    int timeout_sec;
    ProxyInfo proxy;

    explicit Impl(int timeout, const std::string& proxy_url = "")
        : timeout_sec(timeout) {
        ssl_ctx.set_default_verify_paths();
        ssl_ctx.set_verify_mode(ssl::verify_peer);
        proxy = detect_proxy(proxy_url);
    }

    // 对外入口：用 wall-clock guard 包住实际请求。boost::beast 的 expires_after
    // 在某些代理/SSL 异常路径下不生效（实测出现过 28 分钟挂死），这里硬上限兜底。
    HttpResponse request(http::verb method,
                         const std::string& url,
                         const std::string& body,
                         const Headers& extra_headers) {
        auto parsed = parse_url(url);

        spdlog::debug("HTTP {} {}", std::string(http::to_string(method)), url);

        // 硬上限 = 配置 timeout 的 2 倍 + 5s 余量，给 beast 自己的 deadline 优先生效。
        const int hard_cap_sec = timeout_sec * 2 + 5;
        const std::string method_str = std::string(http::to_string(method));

        // packaged_task + detach 模式：std::async 的 future 析构会阻塞，无法 detach。
        // shared_ptr 让 task 跨线程共享生命周期，detach 后内部线程仍持有引用。
        auto task = std::make_shared<std::packaged_task<HttpResponse()>>(
            [this, method, parsed, body, extra_headers]() {
                if (!parsed.https) {
                    return request_plain(method, parsed, body, extra_headers);
                }
                return request_ssl(method, parsed, body, extra_headers);
            });
        auto fut = task->get_future();
        std::thread([task]() { (*task)(); }).detach();

        if (fut.wait_for(std::chrono::seconds(hard_cap_sec)) ==
            std::future_status::ready) {
            return fut.get();  // 内部异常会在这里 rethrow
        }

        spdlog::error("HTTP wall-clock timeout after {}s for {} {}",
                      hard_cap_sec, method_str, url);
        throw std::runtime_error(
            "HTTP wall-clock timeout after " + std::to_string(hard_cap_sec) +
            "s for " + method_str + " " + url);
    }

    // Connect TCP stream to target (direct or via proxy CONNECT tunnel)
    void connect_tcp(asio::io_context& ioc, beast::tcp_stream& tcp_stream,
                     const ParsedUrl& parsed) {
        tcp::resolver resolver(ioc);

        if (proxy.enabled) {
            // Connect to proxy
            auto proxy_results = resolver.resolve(proxy.host, proxy.port);
            tcp_stream.expires_after(std::chrono::seconds(timeout_sec));
            tcp_stream.connect(proxy_results);

            // Send CONNECT request through plain TCP
            std::string connect_host = parsed.host + ":" + parsed.port;
            http::request<http::empty_body> connect_req{
                http::verb::connect, connect_host, 11};
            connect_req.set(http::field::host, connect_host);

            tcp_stream.expires_after(std::chrono::seconds(timeout_sec));
            http::write(tcp_stream, connect_req);

            // Read CONNECT response
            beast::flat_buffer buf;
            http::response<http::empty_body> connect_res;
            http::response_parser<http::empty_body> parser(connect_res);
            // CONNECT response has no body regardless of status
            parser.skip(true);

            tcp_stream.expires_after(std::chrono::seconds(timeout_sec));
            http::read(tcp_stream, buf, parser);

            if (connect_res.result() != http::status::ok) {
                throw std::runtime_error(
                    "Proxy CONNECT failed: " +
                    std::to_string(connect_res.result_int()));
            }
            spdlog::debug("Proxy CONNECT tunnel established");
        } else {
            // Direct connect
            auto results = resolver.resolve(parsed.host, parsed.port);
            tcp_stream.expires_after(std::chrono::seconds(timeout_sec));
            tcp_stream.connect(results);
        }
    }

    HttpResponse request_ssl(http::verb method,
                             const ParsedUrl& parsed,
                             const std::string& body,
                             const Headers& extra_headers) {
        asio::io_context ioc;
        beast::ssl_stream<beast::tcp_stream> stream(ioc, ssl_ctx);

        // SNI
        if (!SSL_set_tlsext_host_name(stream.native_handle(), parsed.host.c_str())) {
            throw beast::system_error(
                beast::error_code(static_cast<int>(::ERR_get_error()),
                                  asio::error::get_ssl_category()),
                "Failed to set SNI hostname");
        }

        connect_tcp(ioc, beast::get_lowest_layer(stream), parsed);

        beast::get_lowest_layer(stream).expires_after(
            std::chrono::seconds(timeout_sec));
        stream.handshake(ssl::stream_base::client);

        auto req = build_request(method, parsed, body, extra_headers);

        beast::get_lowest_layer(stream).expires_after(
            std::chrono::seconds(timeout_sec));
        http::write(stream, req);

        beast::flat_buffer buffer;
        http::response<http::string_body> res;

        beast::get_lowest_layer(stream).expires_after(
            std::chrono::seconds(timeout_sec));
        http::read(stream, buffer, res);

        beast::error_code ec;
        stream.shutdown(ec);

        return to_response(res);
    }

    HttpResponse request_plain(http::verb method,
                               const ParsedUrl& parsed,
                               const std::string& body,
                               const Headers& extra_headers) {
        asio::io_context ioc;
        beast::tcp_stream stream(ioc);

        connect_tcp(ioc, stream, parsed);

        auto req = build_request(method, parsed, body, extra_headers);

        stream.expires_after(std::chrono::seconds(timeout_sec));
        http::write(stream, req);

        beast::flat_buffer buffer;
        http::response<http::string_body> res;

        stream.expires_after(std::chrono::seconds(timeout_sec));
        http::read(stream, buffer, res);

        beast::error_code ec;
        stream.socket().shutdown(tcp::socket::shutdown_both, ec);

        return to_response(res);
    }

    http::request<http::string_body>
    build_request(http::verb method,
                  const ParsedUrl& parsed,
                  const std::string& body,
                  const Headers& extra_headers) {
        http::request<http::string_body> req{method, parsed.target, 11};
        req.set(http::field::host, parsed.host);
        req.set(http::field::user_agent, "polymarket-arb/0.1");
        req.set(http::field::accept, "application/json");

        if (!body.empty()) {
            req.set(http::field::content_type, "application/json");
            req.body() = body;
            req.prepare_payload();
        }

        for (const auto& [key, value] : extra_headers) {
            req.set(key, value);
        }

        return req;
    }

    static HttpResponse to_response(const http::response<http::string_body>& res) {
        HttpResponse resp;
        resp.status_code = res.result_int();
        resp.body = res.body();
        for (const auto& field : res) {
            resp.headers[std::string(field.name_string())] =
                std::string(field.value());
        }
        return resp;
    }
};

HttpClient::HttpClient(int timeout_sec, const std::string& proxy_url)
    : impl_(std::make_unique<Impl>(timeout_sec, proxy_url)) {}

HttpClient::~HttpClient() = default;

HttpResponse HttpClient::get(const std::string& url, const Headers& headers) {
    return impl_->request(http::verb::get, url, "", headers);
}

HttpResponse HttpClient::post(const std::string& url,
                               const std::string& body,
                               const Headers& headers) {
    return impl_->request(http::verb::post, url, body, headers);
}

HttpResponse HttpClient::del(const std::string& url,
                              const std::string& body,
                              const Headers& headers) {
    return impl_->request(http::verb::delete_, url, body, headers);
}

void HttpClient::set_timeout(int sec) {
    impl_->timeout_sec = sec;
}

}  // namespace polymarket::net
