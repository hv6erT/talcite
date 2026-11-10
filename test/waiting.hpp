#pragma once

#include <algorithm>
#include <chrono>
#include <string>
#include <string_view>

#include "talcite/loop.hpp"

#include "tal/testing.hpp"

namespace tal::testing {

template <typename Condition>
void waited(std::string_view file, int line, std::string_view shown, talcite::Loop& loop, Condition done,
            std::chrono::milliseconds patience = std::chrono::seconds{10}) {
    const auto latest = std::chrono::steady_clock::now() + patience;

    while (!done()) {
        const auto now = std::chrono::steady_clock::now();

        if (now >= latest) fell(file, line, std::string{shown});

        if (loop.turn() == 0)
            (void)loop.bell().wait(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::min<std::chrono::steady_clock::duration>(latest - now, std::chrono::milliseconds{10})));
    }
}

}

#define QUIETED(loop) UNTIL((loop), (loop).turn() == 0)
