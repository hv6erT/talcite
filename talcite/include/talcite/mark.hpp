#pragma once

#include <cstddef>
#include <string>

namespace talcite::ui {

struct Mark {
    std::size_t line = 0;
    std::size_t from = 0;
    std::size_t upto = 0;
    std::string role;
};

}
