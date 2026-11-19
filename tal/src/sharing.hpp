#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tal/path.hpp"
#include "tal/share.hpp"
#include "tal/trouble.hpp"

namespace tal::fs::sharing {

[[nodiscard]] std::optional<Share> gvfs_named(std::string_view name);

[[nodiscard]] std::optional<std::string> mounted_as(const std::vector<std::string>& names, const Share& one);

[[nodiscard]] std::string answers(std::string_view user, std::string_view password);

[[nodiscard]] std::vector<std::string> listed(std::string_view said);

[[nodiscard]] Kind by_gio(std::string_view said);

[[nodiscard]] Kind by_windows(std::uint32_t code);

[[nodiscard]] Path& gio();

}
