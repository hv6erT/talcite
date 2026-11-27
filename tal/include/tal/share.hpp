#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tal/path.hpp"
#include "tal/task.hpp"

namespace tal::fs {

struct Share {
    std::string server;
    std::string name;
    std::string rest;
    std::string user;

    friend bool operator==(const Share&, const Share&) = default;
};

struct Connection {
    Share share;
    bool connected = false;
    std::optional<Path> drive{};

    friend bool operator==(const Connection&, const Connection&) = default;
};

[[nodiscard]] std::optional<Share> addressed(std::string_view said);

[[nodiscard]] std::string written(const Share& one);

[[nodiscard]] std::optional<Share> shared(const Path& where);

[[nodiscard]] bool keeps_logins();

[[nodiscard]] Task<Path> reach(Share one);

[[nodiscard]] Task<Path> connect(Share one, std::string user, std::string password, bool remember = false);

[[nodiscard]] Task<Path> connect(Share one, bool again = false);

[[nodiscard]] Task<std::vector<std::string>> shares(Share server);

[[nodiscard]] Task<std::vector<Connection>> connections();

}
