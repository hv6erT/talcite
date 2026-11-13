#pragma once

#include <concepts>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tal/controls.hpp"
#include "tal/event.hpp"
#include "tal/geometry.hpp"
#include "tal/model.hpp"
#include "tal/node.hpp"
#include "tal/style.hpp"
#include "tal/surface.hpp"
#include "tal/views.hpp"

namespace tal {

struct Side {
    std::string text;
    std::vector<Mark> marks;
    std::optional<std::size_t> number;

    friend bool operator==(const Side&, const Side&) = default;
};

struct Pair {
    std::optional<Side> left;
    std::optional<Side> right;

    friend bool operator==(const Pair&, const Pair&) = default;
};

struct Hunk {
    std::string title;
    std::vector<Pair> rows;
    bool tickable = false;

    friend bool operator==(const Hunk&, const Hunk&) = default;
};

[[nodiscard]] std::pair<Side, Side> diffed(std::string_view left, std::string_view right);

[[nodiscard]] std::vector<Hunk> compared(std::string_view left, std::string_view right, int context = 3);

class Hunks {
public:
    Hunks() = default;

    virtual ~Hunks();

    Hunks(const Hunks&) = delete;
    Hunks& operator=(const Hunks&) = delete;

    [[nodiscard]] virtual std::size_t count() const = 0;

    [[nodiscard]] virtual Hunk hunk(std::size_t which) const = 0;

    template <typename E>
        requires std::same_as<E, Changes>
    [[nodiscard]] Hook on(Fn<E> what) {
        return hooked(std::make_shared<detail::Typed<E>>(std::move(what)));
    }

protected:
    void told(Changes what);

private:
    [[nodiscard]] Hook hooked(std::shared_ptr<detail::Slot> slot);

    std::vector<std::shared_ptr<detail::Slot>> slots_;
};

struct Ticked : Event {
    Ticked(std::size_t at, bool given) : hunk(at), on(given) {}

    std::size_t hunk;
    bool on;
};

class Diff : public Scrollable {
public:
    explicit Diff(std::shared_ptr<Hunks> hunks);

    Property<std::size_t> current{*this, Effect::none, 0};
    Property<std::optional<bool>> unified{*this, Effect::layout, std::nullopt};
    Property<std::string> left{*this, Effect::layout, {}};
    Property<std::string> right{*this, Effect::layout, {}};

    void open(std::size_t hunk, bool on);

    [[nodiscard]] bool opened(std::size_t hunk) const;

    void mark(std::size_t hunk, bool on);

    [[nodiscard]] bool marked(std::size_t hunk) const;

    [[nodiscard]] std::vector<std::size_t> marks() const { return marks_; }

    void unmark();

    [[nodiscard]] Hunks& hunks() const { return *hunks_; }

    [[nodiscard]] std::size_t extent() const override;

    [[nodiscard]] std::size_t window() const override { return window_; }

    [[nodiscard]] std::size_t top() const override { return top_; }

    void top(std::size_t hunk) override;

protected:
    [[nodiscard]] Size measure(Limits room) const override;

    void arrange(Area inside) override;

    void paint(Canvas& onto) const override;

private:
    struct Stripe;

    [[nodiscard]] std::vector<Stripe> stripes(std::size_t hunk) const;

    [[nodiscard]] std::vector<Stripe> laid() const;

    [[nodiscard]] bool steady(std::size_t hunk) const;

    [[nodiscard]] std::optional<std::size_t> next(std::size_t from, bool down) const;

    [[nodiscard]] bool heading(std::size_t hunk) const;

    [[nodiscard]] int numbers(const std::vector<Stripe>& shown) const;

    [[nodiscard]] std::pair<int, int> folding(const std::vector<Stripe>& shown, std::size_t hunk) const;

    [[nodiscard]] Sides inset() const;

    void steadied();

    void settle();

    void reveal(std::size_t hunk);

    void slide(long long rows);

    void ticked(std::size_t hunk, bool on);

    void section(bool on);

    void changed(const Changes& what);

    void scrolled();

    std::shared_ptr<Hunks> hunks_;
    std::vector<std::size_t> marks_;
    std::vector<std::size_t> opened_;
    std::size_t top_ = 0;
    int part_ = 0;
    std::size_t window_ = 0;
    int rows_ = 0;
    int wide_ = 0;
    bool unify_ = false;
    bool ticks_ = false;
    Hook told_;
    Hooks hooks_;
};

}
