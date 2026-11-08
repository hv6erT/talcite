#pragma once

#include <compare>
#include <string>
#include <string_view>

namespace tal {

class Path {
public:
    Path() = default;

    explicit Path(std::string_view utf8);

    [[nodiscard]] static Path restored(std::string_view stored);

    [[nodiscard]] std::string text() const;

    [[nodiscard]] std::string_view stored() const { return stored_; }

    [[nodiscard]] bool empty() const { return stored_.empty(); }

    [[nodiscard]] Path operator/(std::string_view name) const;

    [[nodiscard]] Path parent() const;

    [[nodiscard]] std::string_view name() const;

    [[nodiscard]] std::string_view stem() const;

    [[nodiscard]] std::string_view extension() const;

    [[nodiscard]] bool root() const;

    [[nodiscard]] bool absolute() const;

    [[nodiscard]] bool within(const Path& folder) const;

    [[nodiscard]] bool same(const Path& other) const;

    friend bool operator==(const Path&, const Path&) = default;

    friend std::strong_ordering operator<=>(const Path&, const Path&) = default;

private:
    std::string stored_;
};

}
