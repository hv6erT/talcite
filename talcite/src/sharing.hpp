#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "talcite/share.hpp"

// Części udziałów, które da się sprawdzić bez sieci: odczytanie odmowy z kodu Windows i ze zdania
// `gio`, odszukanie zamontowanego udziału wśród katalogów GVfs, odpowiedzi dla `gio mount` i wykaz
// udziałów z `gio list`. Zerowanie hasła (`wiped`) jest publiczne — twa czyści nim swoje kopie.
namespace talcite::fs {

[[nodiscard]] Refused refused_by_windows(std::uint32_t code);

[[nodiscard]] Refused refused_by_gio(std::string_view said);

[[nodiscard]] std::optional<Share> gvfs_named(std::string_view name);

[[nodiscard]] std::optional<std::string> mounted_as(const std::vector<std::string>& names, const Share& one);

[[nodiscard]] std::string answers(std::string_view user, std::string_view password);

[[nodiscard]] std::vector<std::string> listed(std::string_view said);

}
