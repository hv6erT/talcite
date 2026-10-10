#include "talcite/json.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace talcite::json {

// A value is plain data, like the tree in `xml.cpp`: a recursive variant without parsing, which
// belongs to the adapter. It has the six types of the specification with one difference: numbers
// are split into integer and real. JSON has a single "number" type, but a settings file with
// "columns": 2 must go back to disk as 2, not 2.0, and an int64 above 2^53 does not fit in a
// double without loss. Only the adapter sees whether the text had a decimal point or an exponent,
// so only it can decide; the value only has to keep the answer.
//
// `Array` is a vector of a type that is incomplete at the point of declaration. That is legal
// since C++17, and a variant inside the class needs only the vector itself to be complete, not
// its element. `Member` is a separate struct and not a std::pair, because a pair holding an
// incomplete `Value` in a vector inside `Value` would build only by the grace of the standard
// library implementation.

Value::Value(std::nullptr_t) : held_(nullptr) {}

Value::Value(bool given) : held_(given) {}

Value::Value(std::string given) : held_(std::move(given)) {}

// Separate from the string constructor, because otherwise `Value{"x"}` would pick the bool
// constructor: pointer to bool is a standard conversion, pointer to std::string a user-defined
// one, and the first one wins.
Value::Value(const char* given) : held_(std::string{given}) {}

Value::Value(Array given) : held_(std::move(given)) {}

Value::Value(Object given) : held_(std::move(given)) {}

// The kind of value held. The alternatives of the variant are in the same order as `Type`, on
// purpose, so that the discriminator is a single cast and not a chain of holds_alternative.
Type Value::type() const { return static_cast<Type>(held_.index()); }

// Access works as in xml: a pointer, where `nullptr` means "not this type". There are no
// exceptions, because a value has nothing to fail at; whether a missing number where one was
// expected is an error is for the caller to decide. This one gives the boolean.
const bool* Value::boolean() const { return std::get_if<bool>(&held_); }

// The value as an integer, or `nullptr` when it holds something else.
const std::int64_t* Value::integer() const { return std::get_if<std::int64_t>(&held_); }

// The value as a real number, or `nullptr` when it holds something else; an integer is not
// converted here (see `number`).
const double* Value::real() const { return std::get_if<double>(&held_); }

// The value as a string, or `nullptr` when it holds something else.
const std::string* Value::string() const { return std::get_if<std::string>(&held_); }

// The value as an array, or `nullptr` when it holds something else.
const Array* Value::array() const { return std::get_if<Array>(&held_); }

// The value as an object, or `nullptr` when it holds something else.
const Object* Value::object() const { return std::get_if<Object>(&held_); }

// The string, writable in place, or `nullptr` when the value holds something else.
std::string* Value::string() { return std::get_if<std::string>(&held_); }

// The array, writable in place, or `nullptr` when the value holds something else.
Array* Value::array() { return std::get_if<Array>(&held_); }

// The object, writable in place, or `nullptr` when the value holds something else.
Object* Value::object() { return std::get_if<Object>(&held_); }

// The number regardless of how it was written, for callers that do not care. For an integer this
// is a conversion (with a possible loss above 2^53, which the caller accepts by asking for a
// double). Anything that is not a number gives nothing.
std::optional<double> Value::number() const {
    if (const double* one = real(); one != nullptr) return *one;

    if (const std::int64_t* one = integer(); one != nullptr) return static_cast<double>(*one);

    return std::nullopt;
}

// The member of an object with the given name, or `nullptr` when there is none or the value is
// not an object. Keys in an object are unique (the adapter ensures it when decoding, and `insert`
// when building), so the first match is the only one.
Value* member(Value& one, std::string_view name) {
    Object* object = one.object();

    if (object == nullptr) return nullptr;

    for (Member& each : *object)
        if (each.name == name) return &each.value;

    return nullptr;
}

// The read-only form of `member`; the shared lookup does not change the value.
const Value* member(const Value& one, std::string_view name) {
    return member(const_cast<Value&>(one), name);
}

// Inserts or accesses a member: an existing entry is returned, a missing one is appended as null.
// On any other type it throws std::logic_error, because that is a programming mistake, not bad
// data; turning a value into an object is the caller's decision (Store in twa does it explicitly
// when the new shape calls for it).
Value& insert(Value& one, std::string name) {
    Object* object = one.object();

    if (object == nullptr) throw std::logic_error{"Not an object."};

    for (Member& each : *object)
        if (each.name == name) return each.value;

    object->push_back(Member{std::move(name), Value{}});

    return object->back().value;
}

// Removes the member with the given name from an object. False when the value is not an object
// or has no such member.
bool erase(Value& one, std::string_view name) {
    Object* object = one.object();

    if (object == nullptr) return false;

    const auto found = std::find_if(object->begin(), object->end(),
                                    [name](const Member& each) { return each.name == name; });

    if (found == object->end()) return false;

    object->erase(found);

    return true;
}

}
