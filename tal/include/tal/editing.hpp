#pragma once

#include <compare>
#include <concepts>
#include <cstddef>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tal/model.hpp"

namespace tal {

struct Place {
    std::size_t line = 0;
    std::size_t byte = 0;

    friend auto operator<=>(const Place&, const Place&) = default;
};

struct Edited : Event {
    Edited(Place a, Place b, Place c) : from(a), upto(b), end(c) {}

    Place from;
    Place upto;
    Place end;
};

class Text : public Model {
public:
    explicit Text(std::string_view whole = {});

    template <typename E>
        requires(std::same_as<E, Changes> || std::same_as<E, Edited>)
    [[nodiscard]] Hook on(Fn<E> what) {
        if constexpr (std::same_as<E, Changes>) return Model::on<E>(std::move(what));
        else return heard(std::make_shared<detail::Typed<E>>(std::move(what)));
    }

    [[nodiscard]] std::size_t lines() const { return lines_.size(); }

    [[nodiscard]] std::string_view line(std::size_t at) const;

    [[nodiscard]] std::string whole() const;

    [[nodiscard]] std::string between(Place from, Place to) const;

    Place insert(Place at, std::string_view what);

    void erase(Place from, Place to);

    [[nodiscard]] std::size_t rows() const override { return lines_.size(); }

    [[nodiscard]] Entry entry(std::size_t row, std::size_t column) const override;

private:
    void checked(Place at) const;

    [[nodiscard]] Hook heard(std::shared_ptr<detail::Slot> slot);

    void edited(Edited what);

    std::vector<std::string> lines_;
    std::vector<std::shared_ptr<detail::Slot>> edits_;
};

struct Sift {
    bool regex = false;
    bool cased = false;
    bool whole = false;

    friend bool operator==(const Sift&, const Sift&) = default;
};

struct Hit {
    std::size_t from = 0;
    std::size_t upto = 0;

    friend bool operator==(const Hit&, const Hit&) = default;
};

class Comb {
public:
    Comb(std::string what, Sift how);

    [[nodiscard]] bool ready() const { return wrong_.empty(); }

    [[nodiscard]] const std::string& wrong() const { return wrong_; }

    [[nodiscard]] const std::string& what() const { return what_; }

    [[nodiscard]] Sift how() const { return how_; }

    [[nodiscard]] std::optional<Hit> forth(std::string_view where, std::size_t from) const;

    [[nodiscard]] std::optional<Hit> back(std::string_view where, std::size_t upto) const;

    [[nodiscard]] std::vector<Hit> all(std::string_view where) const;

private:
    [[nodiscard]] std::optional<Hit> earliest(std::string_view where, std::size_t from) const;

    std::string what_;
    Sift how_;
    std::optional<std::regex> expression_;
    std::string wrong_;
};

}
