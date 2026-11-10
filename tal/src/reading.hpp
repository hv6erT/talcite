#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace tal::reading {

enum class Kind { null, boolean, number, string, array, object };

struct Value {
    Kind kind = Kind::null;
    bool boolean = false;
    double number = 0;
    std::string text;
    std::vector<Value> items;
    std::vector<std::string> names;
    int line = 1;
    int column = 1;

    [[nodiscard]] const Value* member(std::string_view name) const;
};

[[nodiscard]] Value parsed(std::string_view json);

[[noreturn]] void refused(const Value& where, std::string_view why);

}
