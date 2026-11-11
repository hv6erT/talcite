#pragma once

#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

#include "tal/controls.hpp"
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

int line(Canvas& onto, Spot at, int wide, std::string_view whole, std::size_t from, std::size_t upto, const std::vector<Mark>& marks,
         const std::vector<Look>& marked, Elide elide, Edge justify, const Look& plain, std::string_view dots, text::Rendering how);

}
