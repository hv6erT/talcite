#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "talcite/bytes.hpp"

namespace talcite {

using Feed = std::function<Bytes(std::size_t at, std::size_t most)>;

using Roll = std::function<std::vector<std::string>(std::size_t which, std::size_t most)>;

using Drain = std::function<bool(Bytes what)>;

}
