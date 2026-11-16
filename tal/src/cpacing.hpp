#pragma once

#include <memory>
#include <optional>
#include <string>

#include "tal/bytes.hpp"
#include "tal/crypto.hpp"
#include "tal/peer.hpp"
#include "tal/port.hpp"

namespace tal::net::cpacing {

[[nodiscard]] Bytes prepended(const Bytes& data);

[[nodiscard]] Bytes generator_string(const Bytes& dsi, const Bytes& prs, const Bytes& ci, const Bytes& sid, std::size_t block);

[[nodiscard]] Bytes transcript(const Bytes& ya, const Bytes& ada, const Bytes& yb, const Bytes& adb);

[[nodiscard]] Bytes intermediate(const Bytes& sid, const Bytes& k, const Bytes& told);

[[nodiscard]] std::optional<Bytes> multiplied(crypto::Cipher& cipher, const Bytes& scalar, const Bytes& point);

#ifdef TAL_OWN_CURVE
[[nodiscard]] Bytes elligator2(const Bytes& u);

[[nodiscard]] Bytes generator(const Bytes& prs, const Bytes& ci, const Bytes& sid);

[[nodiscard]] std::shared_ptr<Handshake> made(Side side, Bytes prs, std::string context, Port<crypto::Cipher> cipher, std::optional<Bytes> scalar);
#endif

}
