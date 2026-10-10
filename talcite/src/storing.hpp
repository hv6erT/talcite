#pragma once

#include <string>
#include <string_view>

namespace talcite::fs {

[[nodiscard]] std::string stored_bytes(std::string_view native);

[[nodiscard]] std::string stored_units(std::u16string_view native);

[[nodiscard]] std::string restored_bytes(std::string_view said);

[[nodiscard]] std::u16string restored_units(std::string_view said);

}
