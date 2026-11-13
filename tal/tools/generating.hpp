#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace tal::generating {

struct Source {
    std::string name;
    std::string text;
};

struct Made {
    std::string header;
    std::string code;
};

[[nodiscard]] Made generated(const Source& english, std::string_view space, std::string_view header, std::vector<std::string>& problems);

[[nodiscard]] std::vector<std::string> checked(const Source& english, const std::vector<Source>& languages);

[[nodiscard]] Made settled(const Source& defaults, std::string_view space, std::string_view header, std::vector<std::string>& problems);

}
