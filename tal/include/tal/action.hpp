#pragma once

#include <concepts>
#include <memory>
#include <optional>
#include <string>
#include <typeindex>
#include <typeinfo>
#include <utility>
#include <vector>

#include "tal/event.hpp"
#include "tal/key.hpp"

namespace tal {

class Action;

struct Run : Event {
    explicit Run(Action& one) : action(one) {}

    Action& action;
};

struct Altered : Event {
    explicit Altered(Action& one) : action(one) {}

    Action& action;
};

class Action {
public:
    explicit Action(std::string text, std::optional<char32_t> letter = std::nullopt, std::optional<Key> key = std::nullopt);

    Action(const Action&) = delete;
    Action& operator=(const Action&) = delete;

    void text(std::string said);

    [[nodiscard]] const std::string& text() const { return text_; }

    void letter(std::optional<char32_t> one);

    [[nodiscard]] std::optional<char32_t> letter() const { return letter_; }

    void key(std::optional<Key> one);

    [[nodiscard]] std::optional<Key> key() const { return key_; }

    void checked(std::optional<bool> on);

    [[nodiscard]] std::optional<bool> checked() const { return checked_; }

    void enable(bool on);

    [[nodiscard]] bool enabled() const { return enabled_; }

    template <typename E>
        requires(std::same_as<E, Run> || std::same_as<E, Altered>)
    [[nodiscard]] Hook on(Fn<E> what) {
        return hooked(typeid(E), std::make_shared<detail::Typed<E>>(std::move(what)));
    }

    void run();

private:
    [[nodiscard]] Hook hooked(std::type_index type, std::shared_ptr<detail::Slot> slot);

    void told(Event& one);

    void altered();

    std::string text_;
    std::optional<char32_t> letter_;
    std::optional<Key> key_;
    std::optional<bool> checked_;
    bool enabled_ = true;
    std::vector<std::pair<std::type_index, std::shared_ptr<detail::Slot>>> slots_;
};

}
