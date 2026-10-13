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

struct Rendering {
    bool joined = true;
    bool presented = true;

    friend constexpr bool operator==(Rendering, Rendering) = default;
};

void rendered(Rendering how);

[[nodiscard]] Rendering rendering();

[[nodiscard]] int width(char32_t one);

[[nodiscard]] int width(std::string_view text);

[[nodiscard]] std::size_t cluster_after(std::string_view text, std::size_t at);

[[nodiscard]] std::size_t cluster_before(std::string_view text, std::size_t at);

[[nodiscard]] char32_t glyph(std::string_view cluster);

[[nodiscard]] std::string spelled(char32_t glyph);

[[nodiscard]] bool in_word(char32_t one);

[[nodiscard]] char32_t shown(char32_t one);

[[nodiscard]] std::string to_string(std::wstring_view from);

[[nodiscard]] std::wstring to_wstring(std::string_view from);

}
