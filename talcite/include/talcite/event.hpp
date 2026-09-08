#pragma once

#include <concepts>
#include <cstdint>
#include <string>
#include <utility>

#include "talcite/area.hpp"

namespace talcite::ui {

class Handler;
class Node;

enum class Code : std::uint8_t {
    none,
    escape,
    enter,
    tab,
    backspace,
    insert,
    remove,
    up,
    down,
    left,
    right,
    home,
    end,
    pageup,
    pagedown,
    f1,
    f2,
    f3,
    f4,
    f5,
    f6,
    f7,
    f8,
    f9,
    f10,
    f11,
    f12
};

enum class Press : std::uint8_t { down, up, move };

enum class Mouse : std::uint8_t { none, left, middle, right };

struct Mods {
    bool ctrl = false;
    bool alt = false;
    bool shift = false;

    friend constexpr bool operator==(Mods, Mods) = default;
};

class Key {
public:
    constexpr Key() = default;

    constexpr Key(char32_t letter, Mods with = {}) : held_(letter), with_(with) {}

    constexpr Key(Code code, Mods with = {})
        : held_(code == Code::none ? 0 : apart + static_cast<char32_t>(code)), with_(with) {}

    constexpr Key(Key one, Mods with) : held_(one.held_), with_(with) {}

    [[nodiscard]] constexpr bool held() const { return held_ != 0; }

    [[nodiscard]] constexpr char32_t letter() const { return held_ < apart ? held_ : 0; }

    [[nodiscard]] constexpr Code code() const {
        return held_ < apart ? Code::none : static_cast<Code>(held_ - apart);
    }

    [[nodiscard]] constexpr Mods mods() const { return with_; }

    friend constexpr bool operator==(Key, Key) = default;

private:
    static constexpr char32_t apart = 0x110000;

    char32_t held_ = 0;
    Mods with_;
};

class Event {
public:
    Event() = default;

    virtual ~Event() = default;

    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;
    Event(Event&&) = delete;
    Event& operator=(Event&&) = delete;

    [[nodiscard]] Handler* at() const { return at_; }

private:
    friend class Handler;

    Handler* at_ = nullptr;
};

class KeyEvent : public Event {
public:
    explicit KeyEvent(Key key) : key_(key) {}

    [[nodiscard]] Key key() const { return key_; }

    [[nodiscard]] Mods mods() const { return key_.mods(); }

private:
    Key key_;
};

class MouseEvent : public Event {
public:
    MouseEvent(Spot where, Press press, Mouse button, Mods mods = {})
        : where_(where), press_(press), button_(button), mods_(mods) {}

    [[nodiscard]] Spot where() const { return where_; }

    [[nodiscard]] Press press() const { return press_; }

    [[nodiscard]] Mouse button() const { return button_; }

    [[nodiscard]] Mods mods() const { return mods_; }

private:
    Spot where_;
    Press press_ = Press::move;
    Mouse button_ = Mouse::none;
    Mods mods_;
};

class ClickEvent : public Event {
public:
    ClickEvent(Spot where, Mouse button, Mods mods = {})
        : where_(where), button_(button), mods_(mods) {}

    [[nodiscard]] Spot where() const { return where_; }

    [[nodiscard]] Mouse button() const { return button_; }

    [[nodiscard]] Mods mods() const { return mods_; }

private:
    Spot where_;
    Mouse button_ = Mouse::none;
    Mods mods_;
};

class DoubleClickEvent : public Event {
public:
    DoubleClickEvent(Spot where, Mouse button, Mods mods = {})
        : where_(where), button_(button), mods_(mods) {}

    [[nodiscard]] Spot where() const { return where_; }

    [[nodiscard]] Mouse button() const { return button_; }

    [[nodiscard]] Mods mods() const { return mods_; }

private:
    Spot where_;
    Mouse button_ = Mouse::none;
    Mods mods_;
};

class WheelEvent : public Event {
public:
    WheelEvent(Spot where, int steps, Mods mods = {})
        : where_(where), steps_(steps), mods_(mods) {}

    [[nodiscard]] Spot where() const { return where_; }

    [[nodiscard]] int steps() const { return steps_; }

    [[nodiscard]] Mods mods() const { return mods_; }

private:
    Spot where_;
    int steps_ = 0;
    Mods mods_;
};

class SizeEvent : public Event {
public:
    SizeEvent(int wide, int high) : wide_(wide), high_(high) {}

    [[nodiscard]] int wide() const { return wide_; }

    [[nodiscard]] int high() const { return high_; }

private:
    int wide_ = 0;
    int high_ = 0;
};

class FocusEvent : public Event {
public:
    explicit FocusEvent(Node* other) : other_(other) {}

    [[nodiscard]] Node* other() const { return other_; }

private:
    Node* other_ = nullptr;
};

class UnfocusEvent : public Event {
public:
    explicit UnfocusEvent(Node* other) : other_(other) {}

    [[nodiscard]] Node* other() const { return other_; }

private:
    Node* other_ = nullptr;
};

class ScrollEvent : public Event {
public:
    ScrollEvent() = default;
};

class PickEvent : public Event {
public:
    PickEvent() = default;
};

class CloseEvent : public Event {
public:
    CloseEvent() = default;
};

class AimedEvent : public Event {};

template <std::derived_from<Event> One>
class HotEvent : public AimedEvent {
public:
    explicit HotEvent(One& held) : held_(held) {}

    [[nodiscard]] One& held() const { return held_; }

private:
    One& held_;
};

class PasteEvent : public Event {
public:
    explicit PasteEvent(std::string said) : said_(std::move(said)) {}

    [[nodiscard]] const std::string& said() const { return said_; }

private:
    std::string said_;
};

}
