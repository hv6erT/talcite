#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tal/bytes.hpp"
#include "tal/path.hpp"
#include "tal/port.hpp"
#include "tal/progress.hpp"
#include "tal/task.hpp"
#include "tal/tls.hpp"

namespace tal::net {

struct Header {
    std::string name;
    std::string value;

    friend bool operator==(const Header&, const Header&) = default;
};

struct Url {
    bool secure = false;
    std::string host;
    std::uint16_t port = 0;
    std::string target;

    [[nodiscard]] static std::optional<Url> parsed(std::string_view text);

    [[nodiscard]] std::optional<Url> resolved(std::string_view location) const;

    [[nodiscard]] std::string text() const;

    friend bool operator==(const Url&, const Url&) = default;
};

struct Request {
    std::string method = "GET";
    Url url;
    std::vector<Header> headers;
    Bytes body;
};

struct Response {
    int status = 0;
    std::vector<Header> headers;
    Bytes body;
    Url url;

    [[nodiscard]] const std::string* header(std::string_view name) const;
};

struct Gateway {
    Url url;
    std::string login;

    friend bool operator==(const Gateway&, const Gateway&) = default;
};

struct Proxy {
    std::optional<Gateway> plain;
    std::optional<Gateway> secure;
    std::vector<std::string> bypass;

    [[nodiscard]] static Proxy from_environment();

    [[nodiscard]] static Proxy parsed(std::string_view plain, std::string_view secure, std::string_view bypass);

    [[nodiscard]] const Gateway* chosen(const Url& target) const;
};

struct Limits {
    std::size_t head = 64 * 1024;
    std::size_t body = 16 * 1024 * 1024;
    int redirects = 5;
    std::chrono::milliseconds quiet{10000};
};

class Http {
public:
    explicit Http(Optional<Tls> tls = {}, Limits most = {}, Proxy proxy = Proxy::from_environment());

    [[nodiscard]] Task<Response> fetch(Request one) const;

    [[nodiscard]] Task<Response> fetch(Request one, Path into, Progress* progress = nullptr) const;

    struct Held;

private:
    std::shared_ptr<Held> held_;
};

}
