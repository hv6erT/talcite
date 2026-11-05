#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "talcite/bytes.hpp"
#include "talcite/fs.hpp"
#include "talcite/promise.hpp"

namespace talcite::fs {

struct Share {
    std::string server;
    std::string name;
    std::string rest;
    std::string user;
};

enum class Refused : std::uint8_t { none, login, missing, network, conflict, unsupported, other };

struct Reached {
    Refused refused = Refused::none;
    path where;
    std::string told;
};

struct Offered {
    Refused refused = Refused::none;
    std::vector<std::string> names;
    std::string told;
};

[[nodiscard]] std::optional<Share> addressed(std::string_view said);

[[nodiscard]] std::string written(const Share& one);

[[nodiscard]] std::optional<Share> shared(const path& where);

[[nodiscard]] bool keeps_logins();

[[nodiscard]] Promise<Reached> reach(Share one);

[[nodiscard]] Promise<Reached> connect(Share one, std::string user, std::string password, bool remember);

[[nodiscard]] Promise<Offered> shares(Share server);

void wiped(std::string& secret);

void wiped(Bytes& secret);

}
