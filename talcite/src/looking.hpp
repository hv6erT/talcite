#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <vector>

#include "fscore.hpp"

namespace talcite::fs::core {

struct Sight {
    Kind kind = Kind::none;
    std::uintmax_t size = 0;
    std::chrono::sys_seconds modified{};
    Instant fine{};
};

struct Look {
    std::map<path, Sight> seen;
    std::vector<path> unread;
    bool blind = false;
};

[[nodiscard]] Look looked(const path& where, bool deep);

[[nodiscard]] std::map<path, Sight> filled(Look now, const std::map<path, Sight>& was);

[[nodiscard]] std::vector<Change> apart(const std::map<path, Sight>& was, const std::map<path, Sight>& now);

}
