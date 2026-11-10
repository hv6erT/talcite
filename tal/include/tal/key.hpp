#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace tal {

enum class Code : std::uint8_t {
    none,
    escape,
    enter,
    tab,
    backspace,
    insert,
    remove,
    up,
    down,
    left,
    right,
    home,
    end,
    pageup,
    pagedown,
    f1,
    f2,
    f3,
    f4,
    f5,
    f6,
    f7,
    f8,
    f9,
    f10,
    f11,
    f12,
    f13,
    f14,
    f15,
    f16,
    f17,
    f18,
    f19,
    f20
};

struct Mods {
    bool ctrl = false;
    bool alt = false;
    bool shift = false;

    friend constexpr bool operator==(Mods, Mods) = default;
};

class Key {
public:
    constexpr Key() = default;

    constexpr Key(char32_t letter, Mods with = {}) : held_(letter), with_(with) {}

    constexpr Key(Code code, Mods with = {}) : held_(code == Code::none ? 0 : apart + static_cast<char32_t>(code)), with_(with) {}

    [[nodiscard]] constexpr bool empty() const { return held_ == 0; }

    [[nodiscard]] constexpr char32_t letter() const { return held_ < apart ? held_ : 0; }

    [[nodiscard]] constexpr Code code() const { return held_ < apart ? Code::none : static_cast<Code>(held_ - apart); }

    [[nodiscard]] constexpr Mods mods() const { return with_; }

    [[nodiscard]] static std::optional<Key> from_string(std::string_view written);

    friend constexpr bool operator==(Key, Key) = default;

private:
    static constexpr char32_t apart = 0x110000;

    char32_t held_ = 0;
    Mods with_;
};

[[nodiscard]] std::string to_string(Key one);

[[nodiscard]] bool alike(Key one, Key two);

}
