#pragma once

#include <optional>

namespace tal::lettering {

[[nodiscard]] constexpr char32_t lowered(char32_t one) { return one >= U'A' && one <= U'Z' ? one - U'A' + U'a' : one; }

[[nodiscard]] constexpr bool same(char32_t one, std::optional<char32_t> letter) { return letter && lowered(one) == lowered(*letter); }

}
