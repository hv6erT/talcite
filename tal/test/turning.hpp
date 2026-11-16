#pragma once

#include <optional>

#include "tal/port.hpp"
#include "tal/tls.hpp"
#include "tal/trouble.hpp"

namespace tal::testing {

[[nodiscard]] Port<net::Tls> turned(bool poison = false, std::optional<Kind> refused = std::nullopt);

}
