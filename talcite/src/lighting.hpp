#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace talcite {

// The seam between what lies in a buffer and what gets computed on. `Bytes` holds `std::byte`,
// because that is how every chunk of data looks in talcite (from disk, from the network, from a
// decoder), but there is no arithmetic on `std::byte` and there should not be. Both conversions
// cost zero at run time: `std::byte` is an enumeration with the range of unsigned char, so both
// sides hold the same number in the same register, and the whole cost is paid by the compiler.
//
// This one turns a byte into a number to compute with.
[[nodiscard]] constexpr std::uint8_t sample(std::byte one) {
    return std::to_integer<std::uint8_t>(one);
}

// Turns a computed number back into a byte for a buffer.
[[nodiscard]] constexpr std::byte stored(std::uint8_t one) { return std::byte{one}; }

// A byte in a file is not a brightness. sRGB carries the value along a gamma curve, so two pixels
// with values 0 and 255 have an average brightness of ~0.5, which is byte 188, not 127. Averaging
// raw bytes darkens everything that has an edge: that is the gray mud by which a downscaled image
// is recognized in most programs. This table maps each byte to its linear brightness in [0, 1].
//
// It lives in one place because both scaling and the mosaic compute it; two copies of the same
// table in two translation units are the same one-definition trap as described at `HandPromise`.
inline const std::array<double, 256>& straight() {
    static const std::array<double, 256> made = [] {
        std::array<double, 256> these{};

        for (std::size_t at = 0; at < these.size(); ++at) {
            const double one = static_cast<double>(at) / 255.0;

            these[at] = one <= 0.04045 ? one / 12.92 : std::pow((one + 0.055) / 1.055, 2.4);
        }

        return these;
    }();

    return made;
}

// The way back. Instead of computing the power for every pixel (millions of calls for a
// full-screen image), we search the thresholds between neighboring bytes. The result is rounded
// exactly as the power would round it, and it costs eight comparisons. This table holds the 255
// midpoints between the linear brightnesses of neighboring bytes.
inline const std::array<double, 255>& edges() {
    static const std::array<double, 255> made = [] {
        const std::array<double, 256>& these = straight();

        std::array<double, 255> among{};

        for (std::size_t at = 0; at + 1 < these.size(); ++at)
            among[at] = (these[at] + these[at + 1]) / 2.0;

        return among;
    }();

    return made;
}

// Turns a linear brightness back into an sRGB byte by binary search over `edges`.
[[nodiscard]] inline std::uint8_t bent(double one) {
    const std::array<double, 255>& among = edges();

    const auto found = std::upper_bound(among.begin(), among.end(), one);

    return static_cast<std::uint8_t>(found - among.begin());
}

}
