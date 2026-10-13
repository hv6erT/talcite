#pragma once

#include <cstddef>
#include <functional>

#include "talcite/bytes.hpp"
#include "talcite/promise.hpp"

namespace talcite {

using Feed = std::function<Promise<Bytes>(std::size_t at, std::size_t most)>;

using Drain = std::function<Promise<void>(Bytes what)>;

}
