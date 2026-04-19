#pragma once

#include <map>
#include <memory>
#include <string>

namespace polymarket::net {

struct HttpResponse {
    int status_code = 0;
    std::string body;
    std::map<std::string, std::string> headers;
};

using Headers = std::map<std::string, std::string>;

class HttpClient {
public:
    explicit HttpClient(int timeout_sec = 10, const std::string& proxy_url = "");
    ~HttpClient();

    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    HttpResponse get(const std::string& url, const Headers& headers = {});

    HttpResponse post(const std::string& url,
                      const std::string& body,
                      const Headers& headers = {});

    HttpResponse del(const std::string& url, const Headers& headers = {});

    void set_timeout(int sec);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace polymarket::net
