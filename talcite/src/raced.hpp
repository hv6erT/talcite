#pragma once

#include <chrono>
#include <vector>

#include "talcite/address.hpp"
#include "talcite/patience.hpp"
#include "talcite/promise.hpp"
#include "talcite/stream.hpp"

namespace talcite::net {

// Nawiązanie z pierwszym adresem, który odpowie — wyścig adresów z RFC 8305 („Happy Eyeballs").
// Prywatne: korzysta z niego klient HTTP, a próby sięgają tu po odstęp krótszy niż domyślny.

// Kolejność prób: rodziny na przemian, zaczynając od tej, którą system dał pierwszą.
[[nodiscard]] std::vector<Address> interleaved(std::vector<Address> found);

// Odstęp między kolejnymi próbami, gdy poprzednia ani się nie udała, ani nie odmówiła.
inline constexpr std::chrono::milliseconds stagger{250};

[[nodiscard]] Promise<Stream> raced(std::vector<Address> found, Patience patience,
                                    std::chrono::milliseconds gap = stagger);

}
