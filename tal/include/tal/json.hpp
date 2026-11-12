#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace tal::json {

enum class Kind { null, boolean, integer, number, string, array, object };

struct Limits {
    std::size_t bytes = std::size_t{16} << 20;
    int depth = 64;
    std::size_t values = std::size_t{1} << 20;
    int digits = 400;
};

class Value {
public:
    Value() = default;

    Value(bool one) : held_(one) {}

    template <std::integral T>
        requires(!std::same_as<T, bool> && !std::same_as<T, char>)
    Value(T one) : held_(static_cast<std::int64_t>(one)) {}

    Value(double one) : held_(one) {}

    Value(std::string one) : held_(std::move(one)) {}

    Value(std::string_view one) : held_(std::string{one}) {}

    Value(const char* one) : held_(std::string{one}) {}

    [[nodiscard]] static Value array();

    [[nodiscard]] static Value object();

    [[nodiscard]] Kind kind() const;

    [[nodiscard]] bool boolean() const;

    [[nodiscard]] std::int64_t integer() const;

    [[nodiscard]] double number() const;

    [[nodiscard]] const std::string& string() const;

    [[nodiscard]] std::size_t size() const;

    [[nodiscard]] const Value& at(std::size_t which) const;

    void push(Value one);

    [[nodiscard]] const Value* find(std::string_view name) const;

    void put(std::string name, Value one);

    [[nodiscard]] const std::vector<std::string>& names() const;

    [[nodiscard]] int line() const { return line_; }

    [[nodiscard]] int column() const { return column_; }

    friend bool operator==(const Value& one, const Value& two) { return one.held_ == two.held_; }

private:
    friend class Reader;

    struct Array {
        std::vector<Value> items;

        friend bool operator==(const Array&, const Array&) = default;
    };

    struct Object {
        std::vector<std::string> names;
        std::vector<Value> items;

        friend bool operator==(const Object&, const Object&) = default;
    };

    std::variant<std::monostate, bool, std::int64_t, double, std::string, Array, Object> held_;
    int line_ = 0;
    int column_ = 0;
};

[[nodiscard]] Value parsed(std::string_view text, Limits limits = {});

[[nodiscard]] std::string written(const Value& one, bool indented = false);

}
