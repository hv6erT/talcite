#pragma once

#include <string>
#include <string_view>

namespace talcite::ascii {

[[nodiscard]] char lowered(char one);

[[nodiscard]] std::string lowered(std::string_view text);

[[nodiscard]] std::string_view trimmed(std::string_view text);

}
