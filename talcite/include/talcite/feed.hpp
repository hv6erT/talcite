#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "talcite/bytes.hpp"
#include "talcite/promise.hpp"

namespace talcite {

using Feed = std::function<Promise<Bytes>(std::size_t at, std::size_t most)>;

using Roll = std::function<std::optional<std::vector<std::string>>(std::size_t which, std::size_t most)>;

using Drain = std::function<Promise<void>(Bytes what)>;

}
