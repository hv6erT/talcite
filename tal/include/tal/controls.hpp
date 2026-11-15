#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tal/action.hpp"
#include "tal/document.hpp"
#include "tal/event.hpp"
#include "tal/geometry.hpp"
#include "tal/layout.hpp"
#include "tal/node.hpp"
#include "tal/style.hpp"
#include "tal/surface.hpp"
#include "tal/undo.hpp"

namespace tal {

enum class Elide { none, start, middle, end };

struct Mark {
    std::size_t from = 0;
    std::size_t upto = 0;
    std::string part;
    std::string address;

    friend bool operator==(const Mark&, const Mark&) = default;
};

class Label : public Node {
public:
    explicit Label(std::string said = {});

    Property<std::string> text;
    Property<bool> wrap{*this, Effect::layout, false};
    Property<Edge> justify{*this, Effect::paint, Edge::start};
    Property<Elide> elide{*this, Effect::paint, Elide::none};
    Property<std::vector<Mark>> marks{*this, Effect::paint, {}};

protected:
    [[nodiscard]] Size measure(Limits room) const override;

    void paint(Canvas& onto) const override;

private:
    struct Stretch {
        std::size_t from = 0;
        std::size_t upto = 0;
    };

    [[nodiscard]] std::vector<Stretch> lines(int wide) const;

    [[nodiscard]] std::string linked(Spot at) const;

    Hooks hooks_;
};

class Status : public Label {
public:
    explicit Status(std::string said = {});

protected:
    [[nodiscard]] Size measure(Limits room) const override;
};

class Rule : public Node {
public:
    explicit Rule(Axis axis = Axis::horizontal, std::string title = {});

    Property<std::string> title;
    Property<Border> kind{*this, Effect::paint, Border::thin};

protected:
    [[nodiscard]] Size measure(Limits room) const override;

    void paint(Canvas& onto) const override;

private:
    Axis axis_;
};

class Button : public Node {
public:
    explicit Button(std::shared_ptr<Action> one);

    Property<bool> primary{*this, Effect::layout, false};

    [[nodiscard]] Action& action() const { return *action_; }

protected:
    [[nodiscard]] Size measure(Limits room) const override;

    void paint(Canvas& onto) const override;

private:
    std::shared_ptr<Action> action_;
    Hooks hooks_;
};

class Tick : public Node {
public:
    explicit Tick(std::shared_ptr<Action> one, bool on = false);

    Property<bool> ticked;

    [[nodiscard]] Action& action() const { return *action_; }

protected:
    [[nodiscard]] Size measure(Limits room) const override;

    void paint(Canvas& onto) const override;

private:
    void toggled();

    std::shared_ptr<Action> action_;
    Hooks hooks_;
};

class Choice : public Node {
public:
    explicit Choice(std::vector<std::string> options, std::size_t chosen = 0);

    Property<std::size_t> chosen;

    [[nodiscard]] const std::vector<std::string>& options() const { return options_; }

protected:
    [[nodiscard]] Size measure(Limits room) const override;

    void paint(Canvas& onto) const override;

    [[nodiscard]] std::optional<Caret> caret() const override;

private:
    std::vector<std::string> options_;
    Hooks hooks_;
};

class Select : public Node {
public:
    explicit Select(std::vector<std::string> options, std::size_t chosen = 0);

    Property<std::size_t> chosen;
    Property<std::size_t> rows{*this, Effect::none, 8};

    [[nodiscard]] const std::vector<std::string>& options() const { return options_; }

    void open();

    void close();

    [[nodiscard]] bool opened() const;

protected:
    [[nodiscard]] Size measure(Limits room) const override;

    void paint(Canvas& onto) const override;

private:
    class Drop;

    std::vector<std::string> options_;
    std::weak_ptr<Node> drop_;
    Hooks hooks_;
};

class Gauge : public Node {
public:
    Gauge();

    Property<double> done{*this, Effect::paint, 0.0};
    Property<bool> busy{*this, Effect::paint, false};
    Property<bool> percent{*this, Effect::layout, true};

protected:
    [[nodiscard]] Size measure(Limits room) const override;

    void paint(Canvas& onto) const override;

private:
    void beating();

    Timer beat_;
    int at_ = 0;
    Hooks hooks_;
};

struct Guessing : Event {
    explicit Guessing(std::string said) : text(std::move(said)) {}

    std::string text;
    std::string ahead;
};

struct Typing : Event {
    explicit Typing(char32_t one) : letter(one) {}

    char32_t letter;
};

class Field : public Node {
public:
    explicit Field(std::string said = {});

    Property<std::string> text;
    Property<std::size_t> cursor{*this, Effect::paint, 0};
    Property<bool> hidden{*this, Effect::paint, false};
    Property<Edge> justify{*this, Effect::paint, Edge::start};

    [[nodiscard]] const std::string& guessed() const { return ahead_; }

protected:
    [[nodiscard]] Size measure(Limits room) const override;

    void arrange(Area inside) override;

    void paint(Canvas& onto) const override;

    [[nodiscard]] std::optional<Caret> caret() const override;

    void put(std::string said);

private:
    struct Edit;

    [[nodiscard]] std::size_t at() const;

    [[nodiscard]] int column(std::size_t byte) const;

    [[nodiscard]] int origin() const;

    [[nodiscard]] std::size_t under(int x) const;

    [[nodiscard]] std::size_t word(bool forth) const;

    void typed(std::string_view what);

    void erased(std::size_t from, std::size_t upto);

    void restored(const std::string& made, std::size_t now);

    void moved(std::size_t to);

    void followed();

    void guessing();

    std::string ahead_;
    int shift_ = 0;
    int wide_ = 0;
    bool editing_ = false;
    Undo undo_;
    Hooks hooks_;
};

class HistoryField : public Field {
public:
    explicit HistoryField(std::vector<std::string> earlier = {});

    void remember(std::string one);

    [[nodiscard]] const std::vector<std::string>& history() const { return history_; }

    Property<std::size_t> keeps{*this, Effect::none, 60};

    void open();

    void close();

    [[nodiscard]] bool opened() const;

private:
    class Drop;

    void walked(std::optional<std::size_t> to);

    void kept();

    std::vector<std::string> history_;
    std::optional<std::size_t> walked_;
    std::string typed_;
    std::weak_ptr<Node> drop_;
    Hooks heard_;
};

}
