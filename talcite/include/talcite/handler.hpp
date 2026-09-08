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

using Handle = std::uint64_t;

class Handler {
public:
    Handler();

    virtual ~Handler();

    Handler(const Handler&) = delete;
    Handler& operator=(const Handler&) = delete;
    Handler(Handler&&) = delete;
    Handler& operator=(Handler&&) = delete;

    template <std::derived_from<Event> One>
    Handle on(std::function<bool(One&)> what) {
        return kept(typeid(One), std::make_shared<std::function<bool(One&)>>(std::move(what)));
    }

    template <std::derived_from<Event> One>
    void off(Handle which) {
        freed(typeid(One), which);
    }

    [[nodiscard]] virtual Handler* parent() const;

protected:
    template <std::derived_from<Event> One>
    bool trigger(One& what) {
        if (what.at_ == nullptr) what.at_ = this;

        return carried(what);
    }

    virtual bool react(KeyEvent& what);
    virtual bool react(MouseEvent& what);
    virtual bool react(ClickEvent& what);
    virtual bool react(DoubleClickEvent& what);
    virtual bool react(WheelEvent& what);
    virtual bool react(SizeEvent& what);
    virtual bool react(FocusEvent& what);
    virtual bool react(UnfocusEvent& what);
    virtual bool react(ScrollEvent& what);
    virtual bool react(PickEvent& what);
    virtual bool react(CloseEvent& what);
    virtual bool react(HotEvent<KeyEvent>& what);
    virtual bool react(PasteEvent& what);

private:
    friend class Root;

    template <std::derived_from<Event> One>
    bool carried(One& what) {
        bool widget = true;
        bool caller = true;
        bool took = false;

        for (Handler* at = this; at != nullptr;) {
            Handler* up = at->parent();

            if (widget && at->react(what)) {
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

    Handle kept(std::type_index which, std::shared_ptr<void> what);

    void freed(std::type_index which, Handle one);

    [[nodiscard]] std::vector<std::shared_ptr<void>> among(std::type_index which) const;

    std::unique_ptr<Hands> hands_;
};

}
