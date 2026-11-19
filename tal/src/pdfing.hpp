#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace tal::meta::tagging::pdf {

inline constexpr std::size_t deepest = 64;
inline constexpr std::size_t most_items = std::size_t{1} << 18;

struct Null {
    friend bool operator==(const Null&, const Null&) = default;
};

struct Name {
    std::string text;

    friend bool operator==(const Name&, const Name&) = default;
};

struct Ref {
    std::uint32_t number = 0;
    std::uint32_t generation = 0;

    friend bool operator==(const Ref&, const Ref&) = default;
};

struct Object;
struct Entry;

using Array = std::vector<Object>;

struct Dict {
    std::vector<Entry> entries;

    [[nodiscard]] const Object* find(std::string_view key) const;
};

struct Stream {
    Dict dict;
    std::uint64_t at = 0;
};

struct Object {
    std::variant<Null, bool, std::int64_t, double, Name, std::string, Array, Dict, Ref, Stream> value;
};

struct Entry {
    std::string key;
    Object value;
};

[[nodiscard]] const Dict* dict_of(const Object& one);

[[nodiscard]] const Array* array_of(const Object& one);

[[nodiscard]] const Stream* stream_of(const Object& one);

[[nodiscard]] const std::string* string_of(const Object& one);

[[nodiscard]] const Name* name_of(const Object& one);

[[nodiscard]] std::optional<Ref> ref_of(const Object& one);

[[nodiscard]] std::optional<std::int64_t> integer_of(const Object& one);

[[nodiscard]] std::optional<double> number_of(const Object& one);

[[nodiscard]] bool named(const Object* one, std::string_view text);

class Syntax {
public:
    explicit Syntax(std::span<const std::byte> text, std::size_t at = 0) : text_(text), at_(at) {}

    [[nodiscard]] std::optional<Object> object();

    [[nodiscard]] std::optional<std::int64_t> integer();

    [[nodiscard]] bool keyword(std::string_view word);

    [[nodiscard]] bool eol();

    void blank();

    [[nodiscard]] std::size_t at() const { return at_; }

    [[nodiscard]] bool cut() const { return cut_; }

private:
    [[nodiscard]] std::optional<Object> nested(std::size_t depth);

    [[nodiscard]] std::optional<Object> numeral();

    [[nodiscard]] std::optional<Name> name();

    [[nodiscard]] std::optional<std::string> literal();

    [[nodiscard]] std::optional<std::string> hexadecimal();

    [[nodiscard]] std::string_view word();

    [[nodiscard]] int peek(std::size_t ahead = 0) const;

    std::span<const std::byte> text_;
    std::size_t at_ = 0;
    std::size_t items_ = 0;
    bool cut_ = false;
};

[[nodiscard]] bool spacing(int one);

[[nodiscard]] bool delimiting(int one);

[[nodiscard]] std::string text_of(std::string_view raw);

}
