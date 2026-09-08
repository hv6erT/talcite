#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace talcite {

[[nodiscard]] bool copied(std::string_view what);

[[nodiscard]] std::optional<std::string> pasted();

}
