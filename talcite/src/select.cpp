#include "talcite/select.hpp"

#include <algorithm>
#include <utility>

#include "talcite/encoding.hpp"
#include "talcite/root.hpp"
#include "talcite/skin.hpp"

#include "eliding.hpp"
#include "popup.hpp"

namespace talcite::ui {

namespace {

// The space before the drop-down mark and the mark itself. Always two cells: the text gives way to
// them first, because the mark is what makes this row a selection rather than plain text.
constexpr int aside = 2;

}

// An empty selection with the "talcite.select" role and its drop-down list.
Select::Select() : OptionsMixin("talcite.select"), popup_(std::make_unique<Popup>(*this)) {}

// A selection with the given options, the one at `which` chosen.
Select::Select(std::vector<std::string> options, std::size_t which) : Select() {
    this->options().all(std::move(options));

    this->which(which);
}

Select::~Select() = default;

// How many rows the opened list shows. The opened list lives in `Popup`, which holds both the row
// count and the knowledge of whose veil it is.
int Select::rows() const { return popup_->rows(); }

// Sets how many rows the opened list shows.
void Select::rows(int given) { popup_->rows(given); }

// Whether the list is open right now.
bool Select::opened() const { return popup_->opened(); }

// Closes the list if it is open.
void Select::close() { popup_->close(); }

// Opens the list of options with the current one marked; does nothing when disabled or empty.
void Select::open() {
    if (!enabled() || options().empty()) return;

    // The choice comes back as a value, not a row number. The list is made when it opens, while a
    // number would be resolved only at the press, so if someone changed the options in between,
    // a different string from the one shown would land in the field. A snapshot of the strings sits
    // in the closure, and the strings are unambiguous, because the set allows no duplicates.
    popup_->open(options().all(), *which(),
                 [this, these = options().all()](std::size_t at) { chose(these[at]); });
}

// One row, as wide as the longest option plus the mark, but at most `room`.
Size Select::wants(Size room) const {
    int wide = 0;

    for (const std::string& one : options()) wide = std::max(wide, measured(one).wide);

    return {std::min(wide + aside, room.wide), std::min(1, room.high)};
}

// Makes `given` the value and sends a `PickEvent`, unless it is not an option or already chosen.
void Select::chose(const std::string& given) {
    const std::optional<std::size_t> made = options().find(given);

    if (!made || made == which()) return;

    which(*made);

    (void)trigger(PickEvent{});
}

// Draws the row: the value elided on the right and the drop-down mark at the right edge.
void Select::paint(Canvas& onto) const {
    const Size given = onto.size();

    const Skin& how = skin();

    // A family of roles per state, made of a stem and a suffix: the same contract as in the button
    // and the check box, so that a skin author describes <stem>, <stem>.focus and <stem>.disabled
    // and has nothing to guess.
    const Style look = how.style(!enabled() ? role() + ".disabled" : focused() ? role() + ".focus" : role());

    onto.fill({{0, 0}, {given.wide, 1}}, look);

    if (given.wide <= 0) return;

    // The drop-down mark stands at the right edge, however wide we are made, as in the settings
    // window, where a selection stretches across the whole column.
    onto.put({given.wide - 1, 0}, how.glyph(role() + ".mark"), look);

    const int room = std::max(given.wide - aside, 0);

    if (room <= 0) return;

    onto.put({0, 0},
                elided(value().value_or(std::string{}), room, Elide::right,
                       how.glyph("talcite.elide")),
                look);
}

// Enter opens the list. The side arrows (and Home, End) change the value without opening and
// notify. The up and down arrows are not the selection's: in a window they move between the
// widgets, as in Midnight Commander, and a selection that took them would change its value under
// someone who only wanted to go past it to the next row.
//
// At either end a side arrow goes up the tree too. Letters, Tab and function keys are always passed
// on: if the selection swallowed them, a "Cancel" button would stop answering to its letter
// whenever the focus is here.
bool Select::handle(KeyEvent& what) {
    if (!enabled() || options().empty()) return false;

    if (what.mods().ctrl || what.mods().alt) return false;

    const Code code = what.key().code();

    // Enter alone, not space: the same as in the button and for the same reason.
    if (code == Code::enter) {
        open();

        return true;
    }

    const std::size_t last = options().many() - 1;
    const std::size_t at = *which();

    switch (code) {
        case Code::left:
            if (at == 0) return false;

            chose(options()[at - 1]);

            return true;

        case Code::right:
            if (at == last) return false;

            chose(options()[at + 1]);

            return true;

        case Code::home:
            if (at == 0) return false;

            chose(options()[0]);

            return true;

        case Code::end:
            if (at == last) return false;

            chose(options()[last]);

            return true;

        default: return false;
    }
}

// A click takes the focus and opens the list; it is consumed even when disabled.
bool Select::handle(ClickEvent&) {
    if (!enabled()) return true;

    if (root() != nullptr) focus();

    open();

    return true;
}

// A selection being disabled closes its list. A disabled selection does not choose, and an open
// list would hang over a field that no longer answers. The focus is in the list then, and closing
// the veil returns it to the selection through `focus()`, which a disabled node does not take, so
// the focus moves on by itself.
//
// An empty selection still takes the focus, although an empty option group does not, so
// `takes_focus` is not overridden. An empty group takes zero rows and cannot be seen at all, while
// an empty selection takes a row and is seen together with its drop-down mark. Focus travel that
// skips something visible looks broken.
//
// The event bubbles, but a selection has no children, so every one that arrives here is its own.
// Closing the list is tidying up, not an answer to the event, so the selection does not consume it:
// an ancestor that listens for disabling in its subtree gets this one too.
bool Select::handle(DisableEvent&) {
    close();

    return false;
}

}
