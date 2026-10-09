#pragma once

#include <optional>
#include <string_view>

#include "talcite/address.hpp"

namespace talcite::net {

// Router domyślny z tablicy tras Linuksa (/proc/net/route) — osobno, żeby próba mogła podać tekst
// tablicy wprost, bez zależności od sieci maszyny, na której biegnie.
[[nodiscard]] std::optional<Address> routed(std::string_view table);

}
