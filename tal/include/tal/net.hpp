#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tal/bytes.hpp"
#include "tal/path.hpp"
#include "tal/stream.hpp"
#include "tal/task.hpp"

namespace tal::net {

enum class Family { none, four, six, local };

class Address {
public:
    Address() = default;

    Address(std::string_view text, std::uint16_t port);

    Address(Family family, const std::array<std::byte, 16>& bytes, std::uint16_t port);

    [[nodiscard]] static Address any(Family family, std::uint16_t port);

    [[nodiscard]] static Address loopback(Family family, std::uint16_t port);

    [[nodiscard]] static Address local(const Path& where);

    [[nodiscard]] bool held() const { return family_ != Family::none; }

    [[nodiscard]] Family family() const { return family_; }

    [[nodiscard]] std::uint16_t port() const { return port_; }

    void port(std::uint16_t given);

    [[nodiscard]] std::uint32_t scope() const { return scope_; }

    void scope(std::uint32_t given);

    [[nodiscard]] const std::array<std::byte, 16>& bytes() const { return bytes_; }

    [[nodiscard]] const std::string& path() const { return path_; }

    [[nodiscard]] std::string text() const;

    [[nodiscard]] std::string spelled() const;

    [[nodiscard]] Address unmapped() const;

    friend auto operator<=>(const Address&, const Address&) = default;

private:
    std::array<std::byte, 16> bytes_{};
    std::string path_;
    std::uint16_t port_ = 0;
    std::uint32_t scope_ = 0;
    Family family_ = Family::none;
};

struct Connection {
    Stream stream;
    Address mine;
    Address peer;
    std::optional<bool> kindred;
};

[[nodiscard]] Task<std::vector<Address>> resolve(std::string host, std::uint16_t port, Family family = Family::none);

[[nodiscard]] Task<Connection> connect(Address where);

[[nodiscard]] Task<Connection> connect(std::string host, std::uint16_t port);

class Listener {
public:
    Listener() = default;

    [[nodiscard]] static Task<Listener> opened(Address where, int waiting = 64);

    [[nodiscard]] bool held() const { return held_ != nullptr; }

    [[nodiscard]] Task<Connection> accepted() const;

    [[nodiscard]] Address mine() const;

    void close();

    struct Held;

private:
    explicit Listener(std::shared_ptr<Held> held) : held_(std::move(held)) {}

    std::shared_ptr<Held> held_;
};

struct Datagram {
    Address from;
    Bytes bytes;
    bool cut = false;
};

class Udp {
public:
    Udp() = default;

    [[nodiscard]] static Task<Udp> bound(Address where);

    [[nodiscard]] bool held() const { return held_ != nullptr; }

    [[nodiscard]] Task<void> send(Address to, Bytes what) const;

    [[nodiscard]] Task<Datagram> received(std::size_t most = 65536) const;

    [[nodiscard]] Address mine() const;

    void broadcast(bool given);

    void close();

    struct Held;

private:
    explicit Udp(std::shared_ptr<Held> held) : held_(std::move(held)) {}

    std::shared_ptr<Held> held_;
};

}
