#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "generating.hpp"

namespace tal::generating {

[[nodiscard]] std::optional<Source> sourced(std::string_view program, const std::string& name);

[[nodiscard]] bool spilled(const std::string& name, const std::string& text);

[[nodiscard]] int told(const std::vector<std::string>& problems);

[[nodiscard]] std::string base_name(const std::string& path);

[[nodiscard]] int stored(std::string_view program, const Made& made, const std::string& header, const std::string& code);

}
