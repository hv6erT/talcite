#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "tal/action.hpp"
#include "tal/event.hpp"
#include "tal/geometry.hpp"
#include "tal/layout.hpp"
#include "tal/node.hpp"
#include "tal/surface.hpp"

namespace tal {

enum class Modal { no, yes };

struct Closed : Event {};

class Layers;

class Window : public Node {
public:
    explicit Window(std::string title = {}, Modal modal = Modal::no);

    Property<std::string> title;
    Property<bool> full{*this, Effect::layout, false};

    void seat(std::optional<Area> where);

    [[nodiscard]] std::optional<Area> seat() const { return seat_; }

    [[nodiscard]] Modal modal() const { return modal_; }

    void bind(std::shared_ptr<Action> one);

    void close();

protected:
    [[nodiscard]] Size measure(Limits room) const override;

    void outline(Canvas& box) const override;

private:
    Modal modal_;
    std::optional<Area> seat_;
    std::vector<std::shared_ptr<Action>> bound_;
    Hooks hooks_;
};

class Layers : public Node {
public:
    Layers();

    Window& open(std::shared_ptr<Window> one);

    [[nodiscard]] Window* top() const;

    void raise(Window& one);

    void next();

    void previous();

protected:
    void arrange(Area inside) override;

private:
    friend class Window;

    struct Opened {
        std::weak_ptr<Window> window;
        std::weak_ptr<Node> returning;
        std::weak_ptr<Node> kept;
        Hook arranged;
        bool entered = false;
    };

    void closed(Window& one);

    void entered(Window& one);

    void kept();

    void stepped(bool back);

    [[nodiscard]] Opened* record(const Window& one);

    [[nodiscard]] Window* uppermost(Modal modal, const Window* besides) const;

    [[nodiscard]] std::size_t modals() const;

    std::vector<Opened> opened_;
    Hooks hooks_;
};

class Message : public Window {
public:
    Message(std::string title, std::string text, std::vector<std::shared_ptr<Action>> answers);

private:
    Hooks answered_;
};

class Form : public Window {
public:
    explicit Form(std::string title, Modal modal = Modal::yes);

    [[nodiscard]] Column& body() { return *body_; }

    void answer(std::shared_ptr<Action> one);

private:
    Column* body_ = nullptr;
    Row* answers_ = nullptr;
};

}
