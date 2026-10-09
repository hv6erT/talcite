#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace talcite {

[[nodiscard]] std::string language();

[[nodiscard]] std::string language_of(std::string_view posix);

[[nodiscard]] std::vector<std::string> arguments(int count, char** given);

}
