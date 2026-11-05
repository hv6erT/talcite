#include "talcite/choice.hpp"

#include <algorithm>
#include <utility>

#include "talcite/encoding.hpp"
#include "talcite/root.hpp"
#include "talcite/skin.hpp"

#include "eliding.hpp"


namespace talcite::ui {

namespace {

// The bracket with the mark and a space after it: `(•) `. Four cells, as in a check box.
constexpr int around = 4;

}

// An empty group of radio options, styled as `talcite.choice`.
Choice::Choice() : OptionsMixin("talcite.choice") {}

// A group of radio options with the given one chosen.
Choice::Choice(std::vector<std::string> options, std::size_t which) : Choice() {
    this->options().all(std::move(options));

    this->which(which);
}

// The group takes the focus as a whole, not the single options: Tab enters it once and leaves it
// once, and the arrows move inside. A list has the same shape, and a group in Midnight Commander
// behaves the same way. A group without options takes no focus.
bool Choice::takes_focus() const { return !options().empty(); }

// As wide as the widest option plus the bracket, one row per option, within the room.
Size Choice::wants(Size room) const {
    int wide = 0;

    for (const std::string& one : options()) wide = std::max(wide, measured(one).wide);

    return {std::min(wide + around, room.wide),
            std::min(static_cast<int>(options().many()), room.high)};
}

// We give up the focus lazily, at the layout, not when the set is emptied: `Options` has no pointer
// to its owner, so it has no way to notify it. A group without options is no longer focusable, and
// focus must not stay on a node that does not accept it.
void Choice::layout(Size) {
    if (focused() && !focusable()) unfocus();
}

// One row per option: the bracket with the mark on the chosen one, then the label cut to the width.
void Choice::paint(Canvas& onto) const {
    const Size given = onto.size();

    const Skin& how = skin();

    // The mark comes from the skin, not from the code: a terminal without Unicode gets an asterisk,
    // just as the ellipsis of clipping comes from `talcite.elide`.
    const char32_t dot = how.glyph(role() + ".dot");

    const Style plain = how.style(enabled() ? role() : role() + ".disabled");
    const Style focus = how.style(enabled() ? role() + ".focus" : role() + ".disabled");

    const int room = std::max(given.wide - around, 0);

    for (std::size_t at = 0; at < options().many(); ++at) {
        const int y = static_cast<int>(at);

        if (static_cast<int>(at) >= given.high) break;

        // The chosen option is highlighted, and only when the group has the focus; otherwise a
        // window whose focus is elsewhere would look as if it stood on the group.
        const bool here = at == this->which() && focused();

        const Style look = here ? focus : plain;

        onto.fill({{0, y}, {given.wide, 1}}, look);

        std::string first = "(";

        if (at == this->which())
            utf8::encode(dot, first);
        else
            first += ' ';

        first += ") ";

        onto.put({0, y}, first, look);

        if (room > 0)
            onto.put({around, y},
                        elided(options()[at], room, Elide::right,
                               how.glyph("talcite.elide")),
                        look);
    }
}

// Moving is choosing: an arrow changes the choice at once, as in Midnight Commander, instead of
// moving a separate cursor to confirm with space. Space works anyway, since it is sometimes a
// reflex. Enter, Tab and letters go up; Enter belongs to the window's primary button. A change
// announces a `PickEvent`.
bool Choice::handle(KeyEvent& what) {
    if (!enabled() || options().empty()) return false;

    if (what.mods().ctrl || what.mods().alt) return false;

    const std::size_t was = *which();

    switch (what.key().code()) {
        case Code::up:
            if (was == 0) return false;

            which(was - 1);

            break;

        case Code::down:
            if (was + 1 >= options().many()) return false;

            which(was + 1);

            break;

        // Home and End at the end they lead to move nothing, and the key goes up, as the arrows do.
        case Code::home:
            if (was == 0) return false;

            which(0);

            break;

        case Code::end:
            if (was + 1 >= options().many()) return false;

            which(options().many() - 1);

            break;

        default:
            if (what.key().letter() != U' ') return false;

            break;
    }

    if (*which() == was) return true;

    (void)trigger(PickEvent{});

    return true;
}

// A click takes the focus and chooses the option in the clicked row, announcing a `PickEvent` when
// the choice changed.
bool Choice::handle(ClickEvent& what) {
    if (!enabled() || options().empty()) return true;

    if (root() != nullptr) focus();

    const int y = local(what.where()).y;

    if (y < 0 || static_cast<std::size_t>(y) >= options().many()) return true;

    const std::size_t was = *which();

    which(static_cast<std::size_t>(y));

    if (*which() == was) return true;

    (void)trigger(PickEvent{});

    return true;
}

}
