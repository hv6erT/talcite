#pragma once

#include <string>
#include <string_view>

#include "tal/notify.hpp"
#include "tal/port.hpp"
#include "tal/process.hpp"

namespace tal::notifying {

[[nodiscard]] std::string gvariant(std::string_view text);

[[nodiscard]] std::string applescript(std::string_view text);

[[nodiscard]] Port<Notifier> programs(Port<Processes> processes, bool apple);

}
