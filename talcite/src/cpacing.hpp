#pragma once

#include <memory>
#include <optional>
#include <string>

#include "talcite/bytes.hpp"
#include "talcite/cipher.hpp"
#include "talcite/handshake.hpp"
#include "talcite/seam.hpp"

// CPace na X25519 i SHA-512 (draft-irtf-cfrg-cpace) — części, które sprawdzają wektory CFRG.
namespace talcite::net::cpacing {

[[nodiscard]] Bytes prepended(const Bytes& data);

[[nodiscard]] Bytes generator_string(const Bytes& dsi, const Bytes& prs, const Bytes& ci, const Bytes& sid,
                                     std::size_t block);

[[nodiscard]] Bytes elligator2(const Bytes& u);

[[nodiscard]] Bytes generator(const Bytes& prs, const Bytes& ci, const Bytes& sid);

[[nodiscard]] std::optional<Bytes> multiplied(const Seam<CipherProvider>& cipher, const Bytes& scalar, const Bytes& point);

[[nodiscard]] Bytes transcript(const Bytes& ya, const Bytes& ada, const Bytes& yb, const Bytes& adb);

[[nodiscard]] Bytes intermediate(const Bytes& sid, const Bytes& k, const Bytes& transcript);

[[nodiscard]] std::shared_ptr<Handshake> made(Side side, Bytes prs, std::string context, Seam<CipherProvider> cipher,
                                              std::optional<Bytes> scalar);

}
