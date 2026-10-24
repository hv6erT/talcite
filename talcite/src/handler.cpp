#include "talcite/handler.hpp"

#include "talcite/root.hpp"

#include <algorithm>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace talcite::ui {

// Handlers live in a map from the event type to a vector of number-hand pairs. The function in a hand
// is held through a pointer to void, because the map cannot know types that do not exist yet; the
// map key is the type itself, though, so recovering it on the other side is not guesswork. The lock
// guards the map: a hook may be dropped on another thread than the one the events run on (with the
// object that held it), and dropping it changes the map.
struct detail::Hands {
    std::mutex guard;
    std::unordered_map<std::type_index, std::vector<std::pair<std::uint64_t, std::shared_ptr<Hand>>>> held;
    std::uint64_t next = 0;
};

// A hook unhooks itself: the listener lasts exactly as long as whoever keeps the hook, and a callback
// that captured its keeper can never outlive it. The hook reaches the handler's hands only weakly,
// so a hook that outlives its handler has nothing left to unhook. Moving hands the listener over; the
// hook moved from is empty.
Hook::Hook(Hook&& other) noexcept
    : hands_(std::move(other.hands_)), type_(other.type_), number_(std::exchange(other.number_, 0)) {}

// Unhooks what this hook held, then takes over the other one's listener.
Hook& Hook::operator=(Hook&& other) noexcept {
    if (this != &other) {
        unhooked();

        hands_ = std::move(other.hands_);
        type_ = other.type_;
        number_ = std::exchange(other.number_, 0);
    }

    return *this;
}

// Unhooks the listener, if it still hangs.
Hook::~Hook() { unhooked(); }

// Whether nothing hangs through this hook: it was never given a listener, gave it away, or its
// handler is gone.
bool Hook::empty() const { return number_ == 0 || hands_.expired(); }

// Takes the listener off its handler. A dispatch under way holds its own copy of the hands, so the
// hand is also marked as dead: the dispatch skips it rather than call a listener whose keeper may be
// gone. The listener being called just now finishes its call, since the dispatch keeps it alive.
void Hook::unhooked() {
    const std::uint64_t gone = std::exchange(number_, 0);

    const std::shared_ptr<detail::Hands> hands = hands_.lock();

    hands_.reset();

    if (gone == 0 || !hands) return;

    // The hand goes after the lock: what its callback captured may hold hooks of its own, and those
    // unhook in its destructor.
    std::shared_ptr<detail::Hand> dropped;

    {
        const std::lock_guard<std::mutex> held{hands->guard};

        const auto at = hands->held.find(type_);

        if (at == hands->held.end()) return;

        auto& mine = at->second;

        const auto found = std::find_if(mine.begin(), mine.end(), [gone](const auto& one) { return one.first == gone; });

        if (found == mine.end()) return;

        found->second->live.store(false);
        dropped = std::move(found->second);

        mine.erase(found);

        if (mine.empty()) hands->held.erase(at);
    }
}

Handler::Handler() = default;

// A handler that goes marks its listeners dead. A dispatch under way holds them in its own copy
// (`among`), and a handler destroyed by one of its own listeners must not call the others: what they
// captured may have gone with it. Their hooks find the hands gone and have nothing to unhook.
Handler::~Handler() {
    if (!hands_) return;

    const std::lock_guard<std::mutex> held{hands_->guard};

    for (auto& [type, hands] : hands_->held)
        for (auto& [number, hand] : hands) hand->live.store(false);
}

// A bare handler has no parent, so events stop with it.
Handler* Handler::parent() const { return nullptr; }

// A bare handler belongs to no tree with a root, so its events are no root's delivery.
Root* Handler::root() const { return nullptr; }

// Every event is a delivery of the root of the tree it starts in, whoever starts it: the root with
// a key or the mouse, but also a widget itself (`Button::pick` called by a program, a reaction
// after a promise). During a delivery no node removed from the tree is released (`Root::linger`),
// so a listener may close the window the event is still climbing through, and the climb goes on
// over the nodes as they were. Before, only the root's own deliveries counted, and a press started
// by the program climbed over a window freed under it. The count is a counter, so an event inside
// another one changes nothing; outside a tree with a root there is nothing to count.
void Handler::delivering(Root* top) {
    if (top != nullptr) ++top->busy_;
}

// Ends the delivery begun by `delivering`; the last one releases the nodes removed during it.
void Handler::delivered(Root* top) {
    if (top != nullptr) top->released();
}

// The widget channel knows only the library's events, with one `handle` per type. A program's own
// event (QuitEvent in twa) travels only through the callers' channel, via `on<>`: `carried` checks
// at compile time whether there is a `handle` for it and, when there is none, does not open the
// widget channel. A type derived from a library event without its own overload does not compile:
// `known` says an overload matches, `exact` says it is not an exact one, and the `static_assert`
// makes it plain.
//
// The name is `handle` because the class is called `Handler`; the caller says `on`, the widget says
// `handle`. It comes from the same line as handleEvent in Turbo Vision.
//
// By default a widget does not take a key.
bool Handler::handle(KeyEvent&) { return false; }

// By default a widget does not take a mouse event.
bool Handler::handle(MouseEvent&) { return false; }

// By default a widget does not take a click.
bool Handler::handle(ClickEvent&) { return false; }

// By default a widget does not take a double click.
bool Handler::handle(DoubleClickEvent&) { return false; }

// By default a widget does not take a wheel turn.
bool Handler::handle(WheelEvent&) { return false; }

// By default a widget does not react to a size change.
bool Handler::handle(SizeEvent&) { return false; }

// By default a widget does not react to gaining focus.
bool Handler::handle(FocusEvent&) { return false; }

// By default a widget does not react to losing focus.
bool Handler::handle(UnfocusEvent&) { return false; }

// By default a widget does not react to being enabled.
bool Handler::handle(EnableEvent&) { return false; }

// By default a widget does not react to being disabled.
bool Handler::handle(DisableEvent&) { return false; }

// By default a widget does not take a scroll request.
bool Handler::handle(ScrollEvent&) { return false; }

// By default a widget does not take a pick.
bool Handler::handle(PickEvent&) { return false; }

// By default a widget does not take a close request.
bool Handler::handle(CloseEvent&) { return false; }

// By default a widget does not take a shortcut.
bool Handler::handle(ShortcutEvent&) { return false; }

// By default a widget does not take a paste.
bool Handler::handle(PasteEvent&) { return false; }

// Registers a handler for the event type and gives back the hook that keeps it. The map is
// allocated only on the first registration: a node nobody hooked anything to (most of them) costs
// one pointer, not a map. Numbers start at one, so zero can mean an empty hook.
Hook Handler::kept(std::type_index which, std::shared_ptr<void> what) {
    if (!hands_) hands_ = std::make_shared<detail::Hands>();

    const std::lock_guard<std::mutex> held{hands_->guard};

    const std::uint64_t one = ++hands_->next;

    auto hand = std::make_shared<detail::Hand>();

    hand->what = std::move(what);

    hands_->held[which].emplace_back(one, std::move(hand));

    return Hook{hands_, which, one};
}

// The handlers of one event type, as a copy, not a view. A handler may hook or unhook another
// during its own call, which would rearrange the vector under the loop's feet. An empty vector
// costs nothing, so only a node that really has handlers pays for this.
std::vector<std::shared_ptr<detail::Hand>> Handler::among(std::type_index which) const {
    std::vector<std::shared_ptr<detail::Hand>> made;

    if (!hands_) return made;

    const std::lock_guard<std::mutex> held{hands_->guard};

    const auto at = hands_->held.find(which);
    if (at == hands_->held.end()) return made;

    made.reserve(at->second.size());

    for (const auto& one : at->second) made.push_back(one.second);

    return made;
}

}
