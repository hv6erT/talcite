#pragma once

#include <optional>
#include <string_view>

#include "tal/event.hpp"
#include "tal/geometry.hpp"
#include "tal/key.hpp"
#include "tal/style.hpp"
#include "tal/surface.hpp"
#include "tal/text.hpp"

namespace tal::controlling {

[[nodiscard]] bool lettered(std::string_view cluster, std::optional<char32_t> letter);

int spelled(Canvas& onto, Spot at, std::string_view said, std::optional<char32_t> letter, const Look& plain, const Look& marked,
            text::Rendering how);

[[nodiscard]] bool pressing(Key key);

[[nodiscard]] bool clicking(const MouseEvent& one);

[[nodiscard]] std::string_view lined(Border kind);

}
