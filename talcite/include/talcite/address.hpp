#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "talcite/promise.hpp"
#include "talcite/trouble.hpp"

namespace talcite::net {

enum class Family : std::uint8_t { none, four, six, local };

class Address {
public:
    Address() = default;

    Address(std::string_view text, std::uint16_t port);

    Address(Family family, const std::array<std::byte, 16>& bytes, std::uint16_t port);

    [[nodiscard]] static Address any(Family family, std::uint16_t port);

    [[nodiscard]] static Address loopback(Family family, std::uint16_t port);

    [[nodiscard]] static Address local(std::string path);

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

    friend bool operator==(const Address&, const Address&) = default;

private:
    std::array<std::byte, 16> bytes_{};
    std::string path_;
    std::uint32_t scope_ = 0;
    std::uint16_t port_ = 0;
    Family family_ = Family::none;
};

[[nodiscard]] Promise<std::vector<Address>> resolve(std::string host, std::uint16_t port,
                                                    Family family = Family::none);

[[nodiscard]] Promise<std::vector<Address>> mine(Family family = Family::none);


class Trouble : public talcite::Trouble {
public:
    explicit Trouble(const std::string& said, int code = 0, Address where = {});

    [[nodiscard]] const Address& where() const { return where_; }

private:
    Address where_;
};

[[nodiscard]] std::string spoken(int code);

}
