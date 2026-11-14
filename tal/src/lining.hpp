#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "tal/editing.hpp"
#include "tal/text.hpp"

namespace tal::lining {

enum class Doing { typing, back, forth, other };

template <typename At>
struct Piece {
    At from{};
    std::string gone;
    std::string came;
};

[[nodiscard]] std::size_t ended(std::size_t at, std::string_view what);

[[nodiscard]] Place ended(Place at, std::string_view what);

[[nodiscard]] bool lettered(std::string_view line, std::size_t at);

[[nodiscard]] std::size_t worded(std::string_view line, std::size_t at, bool forth, text::Rendering how);

template <typename At>
[[nodiscard]] bool joined(Doing kind, Piece<At>& last, Doing next, const Piece<At>& one) {
    if (kind != next) return false;

    switch (kind) {
        case Doing::typing:
            if (!one.gone.empty() || one.came.find_first_of(" \t\n") != std::string::npos || one.from != ended(last.from, last.came)) return false;

            last.came += one.came;

            return true;
        case Doing::back:
            if ((one.gone + last.gone).find('\n') != std::string::npos || ended(one.from, one.gone) != last.from) return false;

            last.from = one.from;
            last.gone = one.gone + last.gone;

            return true;
        case Doing::forth:
            if ((last.gone + one.gone).find('\n') != std::string::npos || one.from != last.from) return false;

            last.gone += one.gone;

            return true;
        case Doing::other: return false;
    }

    return false;
}

}
