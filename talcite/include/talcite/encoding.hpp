#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace talcite::utf8 {

[[nodiscard]] std::size_t trailing(unsigned char lead);

[[nodiscard]] char32_t decode(std::string_view from, std::size_t& at);

void encode(char32_t one, std::string& into);

[[nodiscard]] std::string encode(char32_t one);

}

namespace talcite {

[[nodiscard]] int width(char32_t one);

[[nodiscard]] bool in_word(char32_t one);

[[nodiscard]] char32_t shown(char32_t one);

[[nodiscard]] std::string to_string(std::wstring_view from);

[[nodiscard]] std::wstring to_wstring(std::string_view from);

}
