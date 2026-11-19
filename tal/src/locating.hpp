#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tal/path.hpp"

namespace tal::locating {

[[nodiscard]] std::string located(const Path& where);

[[nodiscard]] std::optional<Path> unlocated(std::string_view uri);

[[nodiscard]] std::vector<Path> listed(std::string_view text);

[[nodiscard]] std::optional<std::vector<std::string>> words(std::string_view text, bool windows);

[[nodiscard]] std::optional<std::vector<Path>> dropped(std::string_view text);

}
