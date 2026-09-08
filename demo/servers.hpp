#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace showing {

using Server = std::pair<std::string, std::uint16_t>;

inline std::vector<Server> known() {
    return {{"stun.l.google.com", 19302},
            {"stun.cloudflare.com", 3478},
            {"stun1.l.google.com", 19302},
            {"stun.nextcloud.com", 443}};
}

}
