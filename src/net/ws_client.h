#pragma once

#include <functional>
#include <memory>
#include <string>

namespace polymarket::net {

// 回调类型
using WsMessageCallback = std::function<void(const std::string& msg)>;
using WsErrorCallback = std::function<void(const std::string& error)>;
using WsConnectCallback = std::function<void()>;
using WsCloseCallback = std::function<void()>;

class WsClient {
public:
    explicit WsClient(int timeout_sec = 10);
    ~WsClient();

    WsClient(const WsClient&) = delete;
    WsClient& operator=(const WsClient&) = delete;

    void on_message(WsMessageCallback cb);
    void on_error(WsErrorCallback cb);
    void on_connect(WsConnectCallback cb);
    void on_close(WsCloseCallback cb);

    // 连接到 WSS URL，阻塞直到连接建立
    void connect(const std::string& url);

    // 发送文本消息
    void send(const std::string& msg);

    // 启动消息循环（阻塞，在独立线程中调用）
    void run();

    // 线程安全：请求关闭
    void close();

    bool is_connected() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace polymarket::net
