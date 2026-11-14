#pragma once

#include <cstddef>
#include <optional>
#include <string>
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

[[nodiscard]] Sides boxed(const Style& style);

void unheeded(const Node& from, const Followed& told);

template <typename Draw>
[[nodiscard]] std::string addressed(int wide, int x, text::Rendering how, Draw draw) {
    if (wide <= 0 || x < 0 || x >= wide) return {};

    Surface row{Size{wide, 1}, how};
    Canvas onto = row.canvas(Area{{0, 0}, {wide, 1}});

    draw(onto);

    return std::string{row.address(row.at({x, 0}).link)};
}

}
