#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "tal/json.hpp"
#include "tal/text.hpp"

namespace tal::wording {

enum class Shape { text, letter, counted };

struct Entry {
    std::vector<std::string> road;
    Shape shape = Shape::text;
    int slots = 0;
    const json::Value* english = nullptr;
};

struct Problem {
    int line = 0;
    int column = 0;
    std::string why;
};

[[nodiscard]] int slots(std::string_view said);

[[nodiscard]] bool letter(std::string_view said);

[[nodiscard]] std::string joined(const std::vector<std::string>& road);

[[nodiscard]] bool forms(const json::Value& one);

[[nodiscard]] std::vector<Entry> entries(const json::Value& english, std::vector<Problem>& problems);

[[nodiscard]] const json::Value* found(const json::Value& document, const std::vector<std::string>& road);

[[nodiscard]] std::string unfit(const Entry& entry, const json::Value& given, const text::Language& language);

[[nodiscard]] std::vector<Problem> compared(const json::Value& given, const json::Value& english, const text::Language& language);

[[nodiscard]] bool code(std::string_view said);

}
