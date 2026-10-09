#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace talcite {

using Bytes = std::vector<std::byte>;

[[nodiscard]] Bytes bytes(std::string_view text);

[[nodiscard]] std::string text(std::span<const std::byte> given);

}
