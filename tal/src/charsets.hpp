#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include "tal/docs.hpp"

namespace tal::docs::charsets {

[[nodiscard]] bool whole_utf8(std::span<const std::byte> given);

[[nodiscard]] std::string from_utf8(std::span<const std::byte> given);

[[nodiscard]] std::string from_utf16(std::span<const std::byte> given, bool big);

[[nodiscard]] std::string from_single(std::span<const std::byte> given, Encoding encoding);

[[nodiscard]] char32_t single(std::byte one, Encoding encoding);

}
