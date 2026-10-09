#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "talcite/bytes.hpp"

namespace talcite {

[[nodiscard]] std::string base64(std::span<const std::byte> given);

[[nodiscard]] std::optional<Bytes> unbase64(std::string_view given);

}
