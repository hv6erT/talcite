#include "talcite/handler.hpp"

#include <algorithm>
#include <unordered_map>

namespace talcite::ui {

// Handlers live in a map from the event type to a vector of number-function pairs. The function is
// held through a pointer to void, because the map cannot know types that do not exist yet; the map
// key is the type itself, though, so recovering it on the other side is not guesswork.
struct Handler::Hands {
    std::unordered_map<std::type_index, std::vector<std::pair<std::uint64_t, std::shared_ptr<void>>>>
        held;
    std::uint64_t next = 0;
};

Handler::Handler() = default;

// Defined here, not at the declaration: Hands is incomplete in the header, and unique_ptr must know
// the size of what it frees.
Handler::~Handler() = default;

// A bare handler has no parent, so events stop with it.
Handler* Handler::parent() const { return nullptr; }

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

// Registers a handler for the event type. The map is allocated only on the first registration: a
// node nobody hooked anything to (most of them) costs one pointer, not a map. Numbers start at one,
// so zero can mean an empty hook.
Hook Handler::kept(std::type_index which, std::shared_ptr<void> what) {
    if (!hands_) hands_ = std::make_unique<Hands>();

    const std::uint64_t one = ++hands_->next;

    hands_->held[which].emplace_back(one, std::move(what));

    return Hook{which, one};
}

// The hook knows its type, so unhooking does not need it. After unhooking the hook is empty, so
// `empty()` means exactly "nothing hangs through it"; unhooking an empty one does nothing. A hook
// does not unhook itself when destroyed: most callers throw it away, because the handler is meant
// to live as long as the node.
void Handler::off(Hook& which) {
    const Hook gone = std::exchange(which, Hook{});

    if (!hands_ || gone.empty()) return;

    const auto at = hands_->held.find(gone.type_);
    if (at == hands_->held.end()) return;

    std::vector<std::pair<std::uint64_t, std::shared_ptr<void>>>& mine = at->second;

    mine.erase(std::remove_if(mine.begin(), mine.end(),
                              [&gone](const std::pair<std::uint64_t, std::shared_ptr<void>>& kept) {
                                  return kept.first == gone.number_;
                              }),
               mine.end());

    if (mine.empty()) hands_->held.erase(at);
}

// The handlers of one event type, as a copy, not a view. A handler may hook or unhook another
// during its own call, which would rearrange the vector under the loop's feet. An empty vector
// costs nothing, so only a node that really has handlers pays for this.
std::vector<std::shared_ptr<void>> Handler::among(std::type_index which) const {
    std::vector<std::shared_ptr<void>> made;

    if (!hands_) return made;

    const auto at = hands_->held.find(which);
    if (at == hands_->held.end()) return made;

    made.reserve(at->second.size());

    for (const std::pair<std::uint64_t, std::shared_ptr<void>>& one : at->second)
        made.push_back(one.second);

    return made;
}

}
