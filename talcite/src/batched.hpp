#pragma once

#include <exception>
#include <functional>
#include <vector>

namespace talcite::detail {

[[nodiscard]] std::exception_ptr ran(const std::vector<std::function<void()>>& batch);

}
