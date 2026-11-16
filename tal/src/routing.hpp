#pragma once

#include <optional>
#include <string_view>

#include "tal/net.hpp"

namespace tal::net::routing {

[[nodiscard]] std::optional<Address> routed(std::string_view table);

}
