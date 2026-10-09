#pragma once

#ifdef _WIN32

#include <string>

#include "talcite/child.hpp"

namespace talcite {

[[nodiscard]] std::wstring launched(const Command& what);

}

#endif
