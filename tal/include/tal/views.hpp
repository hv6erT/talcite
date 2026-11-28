#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "tal/event.hpp"
#include "tal/geometry.hpp"
#include "tal/layout.hpp"
#include "tal/model.hpp"
#include "tal/node.hpp"
#include "tal/style.hpp"
#include "tal/surface.hpp"

namespace tal {

struct Picked : Event {
    explicit Picked(std::size_t at) : row(at) {}

    std::size_t row;
};

struct Sorted : Event {
    explicit Sorted(std::size_t at) : column(at) {}

    std::size_t column;
};

struct Scrolled : Event {};

struct Reordered : Event {
    Reordered(std::size_t was, std::size_t now) : from(was), to(now) {}

    std::size_t from;
    std::size_t to;
};

class Scrollable : public Node {
public:
    [[nodiscard]] virtual std::size_t extent() const = 0;

    [[nodiscard]] virtual std::size_t window() const = 0;

    [[nodiscard]] virtual std::size_t top() const = 0;

    virtual void top(std::size_t row) = 0;
};

class List : public Scrollable {
public:
    explicit List(std::shared_ptr<Model> model);

    Property<std::size_t> current{*this, Effect::none, 0};
    Property<bool> reorderable{*this, Effect::none, false};

    void mark(std::size_t row, bool on);

    [[nodiscard]] bool marked(std::size_t row) const;

    [[nodiscard]] std::vector<std::size_t> marks() const { return marks_; }

    void unmark();

    [[nodiscard]] Model& model() const { return *model_; }

    [[nodiscard]] std::size_t extent() const override;

    [[nodiscard]] std::size_t window() const override { return window_; }

    [[nodiscard]] std::size_t top() const override { return top_; }

    void top(std::size_t row) override;

protected:
    [[nodiscard]] Size measure(Limits room) const override;

    void arrange(Area inside) override;

    void paint(Canvas& onto) const override;

    [[nodiscard]] virtual int above() const;

    virtual void drawn(Canvas& onto, std::size_t row, States states) const;

    virtual void clicked(std::size_t row, int x);

    void written(Canvas& onto, const Entry& one, Edge align, States states) const;

    [[nodiscard]] std::size_t shown() const;

    [[nodiscard]] Sides inset() const;

    void redrawn(std::size_t from, std::size_t count);

private:
    [[nodiscard]] std::optional<std::size_t> under(Spot at) const;

    [[nodiscard]] std::size_t nearest(Spot at) const;

    [[nodiscard]] std::size_t placed_at(std::size_t position) const;

    void changed(const Changes& what);

    void lit();

    void moved(std::size_t row, std::size_t view);

    void scrolled();

    std::shared_ptr<Model> model_;
    std::vector<std::size_t> marks_;
    std::size_t top_ = 0;
    std::size_t window_ = 0;
    std::size_t lit_ = 0;
    std::optional<std::size_t> lifting_;
    std::optional<std::size_t> landing_;
    Hook told_;
    Hooks hooks_;
};

struct Heading {
    std::string title;
    Share share;
    Edge align = Edge::start;

    friend bool operator==(const Heading&, const Heading&) = default;
};

class Table : public List {
public:
    Table(std::shared_ptr<Model> model, std::vector<Heading> columns);

    Property<std::vector<Heading>> columns;
    Property<bool> heading{*this, Effect::layout, true};

protected:
    void paint(Canvas& onto) const override;

    [[nodiscard]] int above() const override;

    void drawn(Canvas& onto, std::size_t row, States states) const override;

private:
    [[nodiscard]] std::vector<int> spread(int wide) const;

    Hooks heard_;
};

class Tree : public List {
public:
    explicit Tree(std::shared_ptr<TreeModel> model);

    Property<bool> branches{*this, Effect::paint, false};

protected:
    void drawn(Canvas& onto, std::size_t row, States states) const override;

    void clicked(std::size_t row, int x) override;

private:
    [[nodiscard]] TreeModel& tree() const;

    [[nodiscard]] bool followed(std::size_t row, int depth) const;

    Hooks heard_;
};

class Scroll : public Node {
public:
    explicit Scroll(Scrollable& one);

protected:
    [[nodiscard]] Size measure(Limits room) const override;

    void arrange(Area inside) override;

    void paint(Canvas& onto) const override;

private:
    struct Thumb {
        int from = 0;
        int length = 0;

        friend bool operator==(const Thumb&, const Thumb&) = default;
    };

    [[nodiscard]] Scrollable* view() const;

    [[nodiscard]] std::optional<Thumb> thumb(int high) const;

    [[nodiscard]] int track() const;

    void moved();

    void dragged(int y);

    std::weak_ptr<Node> view_;
    std::optional<Thumb> shown_;
    std::optional<int> grip_;
    Hooks hooks_;
};

}
