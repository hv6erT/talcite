#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace tal::media::lighting {

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

inline const std::array<double, 255>& edges() {
    static const std::array<double, 255> made = [] {
        const std::array<double, 256>& these = straight();
        std::array<double, 255> among{};

        for (std::size_t at = 0; at + 1 < these.size(); ++at) among[at] = (these[at] + these[at + 1]) / 2.0;

        return among;
    }();

    return made;
}

[[nodiscard]] inline unsigned char bent(double one) {
    const std::array<double, 255>& among = edges();

    return static_cast<unsigned char>(std::upper_bound(among.begin(), among.end(), one) - among.begin());
}

}
