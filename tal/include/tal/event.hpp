#pragma once

#include <concepts>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "tal/geometry.hpp"
#include "tal/key.hpp"

namespace tal {

class Node;
class Document;
class Action;

template <typename T>
class Property;

template <typename E>
class Fn {
public:
    Fn() = default;

    template <typename F>
        requires(!std::same_as<std::decay_t<F>, Fn> && std::invocable<std::decay_t<F>&, E&>)
    Fn(F&& what) : held_(std::make_unique<Held<std::decay_t<F>>>(std::forward<F>(what))) {}

    Fn(Fn&&) noexcept = default;
    Fn& operator=(Fn&&) noexcept = default;
    Fn(const Fn&) = delete;
    Fn& operator=(const Fn&) = delete;

    explicit operator bool() const { return held_ != nullptr; }

    void operator()(E& one) { held_->run(one); }

private:
    struct Base {
        virtual ~Base() = default;
        virtual void run(E& one) = 0;
    };

    template <typename F>
    struct Held final : Base {
        explicit Held(F given) : what(std::move(given)) {}

        void run(E& one) override { what(one); }

        F what;
    };

    std::unique_ptr<Base> held_;
};

class Event {
public:
    Event() = default;

    virtual ~Event() = default;

    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;

    void take() { taken_ = true; }

    [[nodiscard]] bool taken() const { return taken_; }

    [[nodiscard]] Node& target() const { return *target_; }

private:
    friend class Node;
    friend class Document;

    bool taken_ = false;
    Node* target_ = nullptr;
};

enum class Phase { capture, bubble };

enum class MouseButton { none, left, middle, right, back, forward };

enum class Press { down, up, move, wheel, click, double_click };

struct KeyEvent : Event {
    explicit KeyEvent(Key given) : key(given) {}

    Key key;
};

struct MouseEvent : Event {
    MouseEvent(Press made, MouseButton which, Mods with, Spot screened, int stepped, Axis along = Axis::vertical)
        : press(made), button(which), mods(with), screen(screened), steps(stepped), axis(along) {}

    Press press;
    MouseButton button;
    Mods mods;
    Spot at;
    Spot screen;
    int steps = 0;
    Axis axis = Axis::vertical;
};

struct PasteEvent : Event {
    explicit PasteEvent(std::string given) : text(std::move(given)) {}

    std::string text;
};

struct Focused : Event {};

struct Blurred : Event {};

struct Arranged : Event {};

struct Changed : Event {
    explicit Changed(const void* source) : source_(source) {}

    template <typename T>
    [[nodiscard]] bool of(const Property<T>& one) const {
        return source_ == static_cast<const void*>(&one);
    }

private:
    const void* source_;
};

namespace detail {

struct Slot {
    virtual ~Slot() = default;

    virtual void called(Event& one) = 0;

    Phase phase = Phase::bubble;
    bool fallback = false;
    bool live = true;
};

template <typename E>
struct Typed final : Slot {
    explicit Typed(Fn<E> given) : what(std::move(given)) {}

    void called(Event& one) override { what(static_cast<E&>(one)); }

    Fn<E> what;
};

}

class Hook {
public:
    Hook() = default;

    ~Hook();

    Hook(Hook&& other) noexcept;
    Hook& operator=(Hook&& other) noexcept;
    Hook(const Hook&) = delete;
    Hook& operator=(const Hook&) = delete;

private:
    friend class Node;
    friend class Action;

    explicit Hook(std::weak_ptr<detail::Slot> slot) : slot_(std::move(slot)) {}

    void released();

    std::weak_ptr<detail::Slot> slot_;
};

class Hooks {
public:
    void add(Hook one);

private:
    std::vector<Hook> held_;
};

}
