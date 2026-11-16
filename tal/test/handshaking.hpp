#pragma once

#include <functional>
#include <memory>

#include "tal/tls.hpp"

namespace tal::testing {

using Making = std::function<std::shared_ptr<net::Tls>(net::Credentials)>;

void handshaken(const Making& made);

}
