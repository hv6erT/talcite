#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace talcite::json {

enum class Type : std::uint8_t { null, boolean, integer, real, string, array, object };

class Value;

struct Member;

using Array = std::vector<Value>;

using Object = std::vector<Member>;

class Value {
public:
    [[nodiscard]] static constexpr std::string_view kind() { return "JSON"; }

    Value() = default;

    Value(std::nullptr_t);

    Value(bool given);

    template <std::integral Given>
        requires(!std::same_as<Given, bool>)
    Value(Given given);

    template <std::floating_point Given>
    Value(Given given);

    Value(std::string given);

    Value(const char* given);

    Value(Array given);

    Value(Object given);

    [[nodiscard]] Type type() const;

    [[nodiscard]] const bool* boolean() const;

    [[nodiscard]] const std::int64_t* integer() const;

    [[nodiscard]] const double* real() const;

    [[nodiscard]] const std::string* string() const;

    [[nodiscard]] const Array* array() const;

    [[nodiscard]] const Object* object() const;

    [[nodiscard]] std::string* string();

    [[nodiscard]] Array* array();

    [[nodiscard]] Object* object();

    [[nodiscard]] std::optional<double> number() const;

    friend bool operator==(const Value&, const Value&) = default;

private:
    std::variant<std::nullptr_t, bool, std::int64_t, double, std::string, Array, Object> held_;
};

struct Member {
    std::string name;
    Value value;

    friend bool operator==(const Member&, const Member&) = default;
};

template <std::integral Given>
    requires(!std::same_as<Given, bool>)
Value::Value(Given given) : held_(static_cast<std::int64_t>(given)) {}

template <std::floating_point Given>
Value::Value(Given given) : held_(static_cast<double>(given)) {}

[[nodiscard]] const Value* member(const Value& one, std::string_view name);

[[nodiscard]] Value* member(Value& one, std::string_view name);

Value& insert(Value& one, std::string name);

bool erase(Value& one, std::string_view name);

}
