#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "talcite/share.hpp"

// The parts of shares that can be tested without a network: reading a refusal from a Windows code
// and from a gio message, finding a mounted share among the GVfs folders, the answers for gio
// mount, and the share list from gio list. Wiping a password (`wiped`) is public: twa clears its
// own copies with it.
namespace talcite::fs {

[[nodiscard]] Refused refused_by_windows(std::uint32_t code);

[[nodiscard]] Refused refused_by_gio(std::string_view said);

[[nodiscard]] std::optional<Share> gvfs_named(std::string_view name);

[[nodiscard]] std::optional<std::string> mounted_as(const std::vector<std::string>& names, const Share& one);

[[nodiscard]] std::string answers(std::string_view user, std::string_view password);

[[nodiscard]] std::vector<std::string> listed(std::string_view said);

}
