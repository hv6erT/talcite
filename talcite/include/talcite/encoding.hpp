#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace talcite {

[[nodiscard]] std::size_t following(unsigned char lead);

[[nodiscard]] char32_t letter(std::string_view from, std::size_t& at);

void spelled(char32_t one, std::string& into);

[[nodiscard]] std::string spelled(char32_t one);

[[nodiscard]] int widely(char32_t one);

[[nodiscard]] bool wordly(char32_t one);

}
