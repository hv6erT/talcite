#pragma once

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tal::text {

struct Span {
    enum class Kind { whole, partial, bad };

    Kind kind = Kind::bad;
    std::size_t size = 0;
};

[[nodiscard]] Span span(std::string_view utf8);

[[nodiscard]] std::optional<char32_t> decoded(std::string_view utf8, std::size_t& at);

void encoded(std::string& into, char32_t point);

[[nodiscard]] bool valid(std::string_view utf8);

[[nodiscard]] std::string repaired(std::string_view bytes);

[[nodiscard]] std::wstring wide(std::string_view utf8);

[[nodiscard]] std::string narrow(std::wstring_view wide);

struct Rendering {
    bool joined = true;
    bool presented = true;

    friend bool operator==(const Rendering&, const Rendering&) = default;
};

[[nodiscard]] int width(char32_t one, Rendering how = {});

[[nodiscard]] int width(std::string_view utf8, Rendering how = {});

[[nodiscard]] std::size_t after(std::string_view utf8, std::size_t at, Rendering how = {});

[[nodiscard]] std::size_t before(std::string_view utf8, std::size_t at, Rendering how = {});

[[nodiscard]] std::vector<std::string_view> clusters(std::string_view utf8, Rendering how = {});

[[nodiscard]] std::vector<std::string_view> wrapped(std::string_view utf8, int columns, Rendering how = {});

[[nodiscard]] bool in_word(char32_t one);

[[nodiscard]] char32_t shown(char32_t one);

[[nodiscard]] char32_t folded(char32_t one);

[[nodiscard]] std::string folded(std::string_view utf8);

[[nodiscard]] char32_t lowered(char32_t one);

[[nodiscard]] std::string lowered(std::string_view utf8);

[[nodiscard]] std::string_view trimmed(std::string_view text);

class Language {
public:
    explicit Language(std::string_view tag);

    [[nodiscard]] std::string_view tag() const { return tag_; }

    [[nodiscard]] std::string_view base() const;

    friend bool operator==(const Language&, const Language&) = default;

private:
    std::string tag_;
};

[[nodiscard]] Language current();

void choose(Language which);

[[nodiscard]] std::optional<Language> proposed();

enum class Case { ignored, considered };

class Collation {
public:
    explicit Collation(Language which = current(), Case cased = Case::ignored, bool numeric = true);

    [[nodiscard]] std::strong_ordering compare(std::string_view one, std::string_view two) const;

    [[nodiscard]] std::string key(std::string_view text) const;

    [[nodiscard]] const Language& language() const { return language_; }

private:
    Language language_;
    Case cased_;
    bool numeric_;
    int tailored_;
};

enum class Plural { zero, one, two, few, many, other };

[[nodiscard]] Plural plural(std::uint64_t many, Language which = current());

[[nodiscard]] std::vector<Plural> plurals(Language which = current());

[[nodiscard]] std::string number(double value, int decimals, Language which = current());

[[nodiscard]] std::string size(std::uint64_t bytes, Language which = current());

enum class Style { date, time, stamp };

[[nodiscard]] std::string date(std::chrono::system_clock::time_point when, Style style, Language which = current());

}
