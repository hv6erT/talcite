#pragma once

#include <cstddef>
#include <ranges>
#include <span>

// The two things every file of talcite holding a secret needs alike, each written once: zeroing a
// secret so that it stays zeroed, and comparing secrets without the time telling where they differ.

namespace talcite::secret {

// Zeroes every element through a volatile pointer, so that the compiler cannot drop the writes as
// stores nobody reads afterwards: for secrets and everything derived from them. The size stays.
template <std::ranges::contiguous_range Held>
void wiped(Held& one) {
    using Element = std::ranges::range_value_t<Held>;

    volatile Element* const at = std::ranges::data(one);
    const auto much = static_cast<std::size_t>(std::ranges::size(one));

    for (std::size_t which = 0; which < much; ++which) at[which] = Element{};
}

// Equality in constant time: the time depends on how many bytes there are, never on where they
// differ. Different lengths are unequal at once, since a length is no secret.
[[nodiscard]] inline bool alike(std::span<const std::byte> one, std::span<const std::byte> two) {
    if (one.size() != two.size()) return false;

    std::byte differ{0};

    for (std::size_t at = 0; at < two.size(); ++at) differ |= one[at] ^ two[at];

    return differ == std::byte{0};
}

}
