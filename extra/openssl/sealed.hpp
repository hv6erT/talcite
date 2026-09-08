#pragma once

#include <memory>
#include <string>

#include "talcite/secure.hpp"

namespace talcite::sealed {

struct Setting {
    std::string certificate;
    std::string key;
    std::string trusted;
    bool checking = true;
};

[[nodiscard]] net::Guard guarding(Setting setting = {});

}
