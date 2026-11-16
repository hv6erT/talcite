#pragma once

#include <memory>

#include "tal/crypto.hpp"
#include "tal/peer.hpp"

namespace tal::testing {

void talked(net::Handshake& calling, net::Handshake& answering);

void agreed(const std::shared_ptr<crypto::Cipher>& cipher);

}
