#pragma once

#include <string>

#include "talcite/child.hpp"
#include "talcite/promise.hpp"

namespace talcite::login {

[[nodiscard]] Promise<bool> enrolled(std::string name);

[[nodiscard]] Promise<void> enrol(std::string name, Command what);

[[nodiscard]] Promise<void> withdraw(std::string name);

}
