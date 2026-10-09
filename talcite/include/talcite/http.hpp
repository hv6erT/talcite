#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "talcite/address.hpp"
#include "talcite/bytes.hpp"
#include "talcite/patience.hpp"
#include "talcite/promise.hpp"
#include "talcite/seam.hpp"
#include "talcite/tls.hpp"

namespace talcite::net {

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

    [[nodiscard]] static std::optional<Url> from_string(std::string_view text);

    [[nodiscard]] std::optional<Url> resolved(std::string_view location) const;

    friend bool operator==(const Url&, const Url&) = default;
};

[[nodiscard]] std::string to_string(const Url& one);

struct Request {
    Url url;
    std::vector<Header> headers;
    std::string method = "GET";
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

    [[nodiscard]] static Proxy from_strings(std::string_view plain, std::string_view secure, std::string_view bypass);

    [[nodiscard]] const Gateway* chosen(const Url& target) const;
};

struct Limits {
    std::size_t head = 64 * 1024;
    std::size_t body = 16 * 1024 * 1024;
    int redirects = 5;
    Patience patience{10000};
};

class HttpTrouble : public Trouble {
public:
    enum class Why : std::uint8_t { malformed, oversized, redirected, downgraded, insecure, proxy };

    HttpTrouble(Why why, const std::string& said, Address where = {});

    [[nodiscard]] Why why() const { return why_; }

private:
    Why why_;
};

class HttpClient {
public:
    struct Held;

    HttpClient() = default;

    explicit HttpClient(Seam<TlsProvider> tls, Limits limits = {}, Proxy proxy = {});

    [[nodiscard]] bool held() const { return held_ != nullptr; }

    [[nodiscard]] Promise<Response> get(Request request) const;

    [[nodiscard]] Promise<Response> post(Request request, Bytes body) const;

private:
    std::shared_ptr<Held> held_;
};

}
