#pragma once

#include <chrono>
#include <vector>

#include "talcite/address.hpp"
#include "talcite/patience.hpp"
#include "talcite/promise.hpp"
#include "talcite/stream.hpp"

namespace talcite::net {

[[nodiscard]] std::vector<Address> interleaved(std::vector<Address> found);

inline constexpr std::chrono::milliseconds stagger{250};

[[nodiscard]] Promise<Stream> raced(std::vector<Address> found, Patience patience,
                                    std::chrono::milliseconds gap = stagger);

}
