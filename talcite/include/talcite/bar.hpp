#pragma once

#include <cstddef>
#include <string>

#include "talcite/area.hpp"
#include "talcite/view.hpp"

namespace talcite::ui {

class Skin;

void barred(View& into, Spot at, int much, bool upright, int shift, int whole, int window,
            const Skin& how, const std::string& role);

void edged(View& into, Size given, Area seen, bool upright, int much, std::size_t shift,
           std::size_t whole, std::size_t window, const Skin& how, const std::string& role);

}
