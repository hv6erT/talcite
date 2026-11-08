#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tal {

enum class Level { debug, info, warning, error };

void log(Level level, std::string_view said);

[[nodiscard]] std::vector<std::string> recent();

namespace detail {

void unhandled(std::string_view said);

void tolerate(int change);

[[nodiscard]] std::uint64_t unhandled_count();

}

}
