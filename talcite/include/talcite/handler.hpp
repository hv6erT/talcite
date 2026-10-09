#pragma once

#include <concepts>
#include <cstdint>
#include <functional>
#include <memory>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <utility>
#include <vector>

#include "talcite/event.hpp"

namespace talcite::ui {

class Root;

class Hook {
public:
    Hook() = default;

    [[nodiscard]] bool empty() const { return number_ == 0; }

private:
    friend class Handler;

    Hook(std::type_index type, std::uint64_t number) : type_(type), number_(number) {}

    std::type_index type_ = typeid(void);
    std::uint64_t number_ = 0;
};

class Handler {
public:
    Handler();

    virtual ~Handler();

    Handler(const Handler&) = delete;
    Handler& operator=(const Handler&) = delete;
    Handler(Handler&&) = delete;
    Handler& operator=(Handler&&) = delete;

    template <std::derived_from<Event> One>
    Hook on(std::function<bool(One&)> what) {
        return kept(typeid(One), std::make_shared<std::function<bool(One&)>>(std::move(what)));
    }

    void off(Hook& which);

    [[nodiscard]] virtual Handler* parent() const;

protected:
    template <std::derived_from<Event> One>
    bool trigger(One& what) {
        if (what.target_ == nullptr) what.target_ = this;

        return carried(what);
    }

    template <std::derived_from<Event> One>
    bool trigger(One&& what) {
        return trigger(static_cast<One&>(what));
    }

    virtual bool handle(KeyEvent& what);
    virtual bool handle(MouseEvent& what);
    virtual bool handle(ClickEvent& what);
    virtual bool handle(DoubleClickEvent& what);
    virtual bool handle(WheelEvent& what);
    virtual bool handle(SizeEvent& what);
    virtual bool handle(FocusEvent& what);
    virtual bool handle(UnfocusEvent& what);
    virtual bool handle(EnableEvent& what);
    virtual bool handle(DisableEvent& what);
    virtual bool handle(ScrollEvent& what);
    virtual bool handle(PickEvent& what);
    virtual bool handle(CloseEvent& what);
    virtual bool handle(ShortcutEvent& what);
    virtual bool handle(PasteEvent& what);

private:
    friend class Root;

    template <class One>
    [[nodiscard]] static consteval bool known() {
        return requires(Handler& at, One& what) { at.handle(what); };
    }

    template <class One>
    [[nodiscard]] static consteval bool exact() {
        return requires { static_cast<bool (Handler::*)(One&)>(&Handler::handle); };
    }

    template <std::derived_from<Event> One>
    bool carried(One& what) {
        static_assert(!known<One>() || exact<One>(),
                      "talcite::ui::Handler: an event derived from a library event needs its own handle");

        bool widget = known<One>();
        bool caller = true;
        bool took = false;

        for (Handler* at = this; at != nullptr;) {
            Handler* up = at->parent();

            if constexpr (known<One>())
                if (widget && at->handle(what)) {
                    widget = false;
                    took = true;
                }

            if (caller && at->handed(what)) {
                caller = false;
                took = true;
            }

            if (!widget && !caller) break;

            if constexpr (std::is_base_of_v<AimedEvent, One>) break;

            at = up;
        }

        return took;
    }

    template <std::derived_from<Event> One>
    bool handed(One& what) {
        bool took = false;

        for (const std::shared_ptr<void>& one : among(typeid(One)))
            if ((*std::static_pointer_cast<std::function<bool(One&)>>(one))(what)) took = true;

        return took;
    }

    struct Hands;

    Hook kept(std::type_index which, std::shared_ptr<void> what);

    [[nodiscard]] std::vector<std::shared_ptr<void>> among(std::type_index which) const;

    std::unique_ptr<Hands> hands_;
};

}
