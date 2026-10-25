#include "talcite/historyfield.hpp"

#include <algorithm>
#include <utility>

#include "popup.hpp"

namespace talcite::ui {

// No default shortcut: the library has no right to occupy a key combination nobody asked it for.
// Whoever wants to open the list from the keyboard adds a key through `keys()`; the arrows walk the
// history without it, because that is movement within the field's own content, not an action.
HistoryField::HistoryField(int wide, std::string text)
    : KeyedMixin<Field>(wide, std::move(text)), popup_(std::make_unique<Popup>(*this)) {}

HistoryField::~HistoryField() = default;

// The hotkey's action: opening the list. In the same way `activate()` presses a menu item, picks in
// a key bar item, and toggles the menu bar.
void HistoryField::activate() { open(); }

// The set of options is public, and the caller adds through it: `options().insert(0, one)` puts a
// command at the front, and a repeat moves to the front instead of appearing twice, because the set
// allows no duplicates. The history has no `add` of its own: it would be a second name for the same
// thing.
//
// It cleans up after itself, on its own side, instead of asking to be notified: it trims to
// `keeps()` and, when it sees that the set has grown or shrunk since the last look, ends the walk
// through the past. It is called from painting and from key handling, so there is no gap between a
// change and its first use.
//
// A change after which the set has as many entries as before goes unnoticed, and the cursor stays
// where it was. That is the only case in which this cleanup does not work, and it is harmless: the
// walk is clamped to the size at every step anyway.
void HistoryField::settle() {
    if (static_cast<int>(options_.many()) > keeps_) {
        std::vector<std::string> these = options_.all();

        these.resize(static_cast<std::size_t>(keeps_));

        options_.all(std::move(these));
    }

    if (options_.many() == seen_) return;

    seen_ = options_.many();
    walked_ = options_.many();
    typed_.clear();
}

// Settles the history first, then paints like a plain field.
View HistoryField::paint(Size given, Area seen) {
    settle();

    return Field::paint(given, seen);
}

// Closes the list and forgets every entry, together with the walk and the saved typed text.
void HistoryField::clear() {
    close();

    options_.clear();

    seen_ = 0;
    walked_ = 0;
    typed_.clear();
}

// Limits the history to `given` entries (at least one). A history without a bottom grows until the
// program ends, and when saved to a file, the file grows too. Older entries fall off the end,
// because they are the least needed.
void HistoryField::keeps(int given) {
    keeps_ = std::max(given, 1);

    if (static_cast<int>(options_.many()) > keeps_) {
        std::vector<std::string> these = options_.all();

        these.resize(static_cast<std::size_t>(keeps_));

        options_.all(std::move(these));
    }

    walked_ = std::min(walked_, options_.many());
}

// The opened list lives in `Popup`, the same one as in a selection. This is the number of rows it
// shows.
int HistoryField::rows() const { return popup_->rows(); }

// Sets the number of rows the opened list shows.
void HistoryField::rows(int given) { popup_->rows(given); }

// Whether the list is open.
bool HistoryField::opened() const { return popup_->opened(); }

// Closes the list, if it is open.
void HistoryField::close() { popup_->close(); }

// Opens the list of past entries with the current one highlighted; does nothing when disabled.
void HistoryField::open() {
    settle();

    if (!enabled()) return;

    // The row number is the entry's number in our own table.
    popup_->open(options_.all(), walked_, [this](std::size_t at) { took(at); });
}

// The only place where an entry from the past reaches the field, and therefore the only one that
// announces it. By arrow or from the list makes no difference: it is the same action.
//
// `Field::text` itself puts the cursor at the end, so typing can continue right after an entry is
// taken.
void HistoryField::took(std::size_t which) {
    if (which >= options_.many()) return;

    // What the user was typing is saved before the field is overwritten, and only on entering the
    // past: later steps must return to the same text, not to the previous entry.
    if (walked_ >= options_.many()) typed_ = text();

    walked_ = which;

    text(options_[which]);

    (void)trigger(PickEvent{});
}

// Returns to what the user was typing. It does not announce anything: nothing from the past reached
// the field, quite the opposite. Without this one step the history is a trap: once in, there is no
// way back to one's own line.
void HistoryField::back() {
    if (walked_ >= options_.many()) return;

    walked_ = options_.many();

    text(typed_);

    typed_.clear();
}

// Up goes into the past, down comes back. The opening key opens the list. Everything else (letters,
// Tab, Home, End, deleting) falls through to the field and works exactly as there.
//
// An empty history takes nothing: there is nothing to walk, so the key has to go up instead of
// vanishing. The same at either end of the walk.
bool HistoryField::handle(KeyEvent& what) {
    settle();

    if (!enabled() || options_.empty()) return Field::handle(what);

    const Mods mods = what.mods();

    // The opening key lies in the key set, as in every other widget. Case folding, exact Ctrl and
    // Alt, and lenient Shift are all done by `contains`, so there is no matching rule of our own
    // here.
    if (keys().contains(what.key())) {
        activate();

        return true;
    }

    if (mods.ctrl || mods.alt) return Field::handle(what);

    const std::size_t many = options_.many();

    switch (what.key().code()) {
        case Code::up:
            // From "nowhere" we step onto the newest entry; further up means further into the past.
            if (walked_ >= many) {
                took(0);

                return true;
            }

            if (walked_ + 1 >= many) return Field::handle(what);

            took(walked_ + 1);

            return true;

        case Code::down:
            if (walked_ >= many) return Field::handle(what);

            if (walked_ == 0) {
                back();

                return true;
            }

            took(walked_ - 1);

            return true;

        default: return Field::handle(what);
    }
}

// A history being disabled closes its list; the same rule as in a selection and for the same
// reason: a disabled widget does not pick, and an open list would hang over a field that no longer
// responds. On closing, focus returns to the nearest willing ancestor, because the history no
// longer takes it. The event is not taken: closing is tidying up after ourselves, and an ancestor
// listening for disabling has to receive it.
bool HistoryField::handle(DisableEvent&) {
    close();

    return false;
}

}
