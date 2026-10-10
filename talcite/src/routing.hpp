#pragma once

#include <optional>
#include <string_view>

#include "talcite/address.hpp"

namespace talcite::net {

[[nodiscard]] std::optional<Address> routed(std::string_view table);

}
