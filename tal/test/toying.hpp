#pragma once

#include <memory>

#include "tal/crypto.hpp"

namespace tal::testing {

[[nodiscard]] std::shared_ptr<crypto::Cipher> toy();

}
