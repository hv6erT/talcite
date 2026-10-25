#include "talcite/keybaritem.hpp"

#include <string>
#include <utility>

#include "talcite/encoding.hpp"
#include "talcite/skin.hpp"

#include "eliding.hpp"

#include "marking.hpp"

namespace talcite::ui {

// A key bar item for one key and its label, with the role "talcite.legend" (the name skins know).
KeyBarItem::KeyBarItem(Key key, std::string text) : text_(std::move(text)) {
    role("talcite.legend");

    keys().add(key);
}

// A key bar item never takes the focus, even when enabled: it answers to its hotkey and to a
// click, and Tab moves through the window's content, not through the key bar.
bool KeyBarItem::takes_focus() const { return false; }

// An item has nothing to hold inside.
int KeyBarItem::capacity() const { return 0; }

// The width comes from the bar; the height is always one row.
Size KeyBarItem::wants(Size room) const { return {room.wide, 1}; }

// The key tag that is drawn: the number of a function key or the letter, or nothing. It is
// derived from the key rather than stored beside it in the item. Two sources of the same truth
// drift apart at the first change of a shortcut; this way a key bar cannot lie about what to press.
std::string KeyBarItem::tag() const {
    // An item is shown by one key, the first in the set. The set may hold more, as every keyed
    // node does, but there is one tag and it cannot show two keys at once.
    if (keys().empty()) return {};

    const Key which = keys().front();

    const Code code = which.code();

    if (code >= Code::f1 && code <= Code::f20)
        return std::to_string(static_cast<int>(code) - static_cast<int>(Code::f1) + 1);

    if (const char32_t letter = which.letter(); letter != 0) return utf8::encode(letter);

    // Escape, Enter and the other named keys have no tag in Midnight Commander, so none is made
    // up here either. An item without a tag shows only the name of the action.
    return {};
}

// Fires a `PickEvent` on the item, unless it is disabled.
void KeyBarItem::pick() {
    if (!enabled()) return;

    (void)trigger(PickEvent{});
}

// The action of an item is picking it, and that is the whole reason the item derives from the
// keyed node. Matching the key, folding letter case and skipping disabled items are done by
// `KeyedMixin`; only what is specific to the key bar is left here.
void KeyBarItem::activate() { pick(); }

// Paints the item in the Midnight Commander look: the number in the plain style, the name in
// reverse video. The number then reads as "press this" and the name as a button to click. The
// reverse video covers the padding to the end of the item, so neighboring items touch and the
// bar has no gaps.
//
// The tag and the hotkey letter are two ways to the same thing, so both are never drawn at once.
// A function key has no letter to underline and gets the number at the side, as in MC. A letter
// key that occurs in the name replaces the tag with an underline, as in a menu item: the same
// thing twice in an item six cells wide would be waste and noise. A letter that does not occur in
// the name goes back to the tag; otherwise it would be a shortcut nobody learns about.
View KeyBarItem::paint(Size given, Area seen) {
    View made{seen.size};

    // The number, the hotkey letter and the name have separate roles, so the skin decides each
    // one on its own. The built-in skin gives the number the plain style, the letter an underline
    // and the name reverse video, which is what MC does on a bare terminal.
    const Skin& how = skin();

    const Style gone = enabled() ? Style{} : how.style(role() + ".disabled");

    const Style plain = Style::merge(how.style(role() + ".tag"), gone);
    const Style label = Style::merge(how.style(role() + ".text"), gone);

    made.fill({{-seen.at.x, -seen.at.y}, given}, plain);

    const char32_t dots = how.glyph("talcite.elide");

    // First the text trimmed to the whole width is examined, because that is what it will be
    // when the tag is dropped. The order is forced: only this result decides how much room is
    // left for the name, and the width of the name decides where the letter really stands.
    const std::string whole = elided(text_, given.wide, Elide::right, dots);

    // The tag is dropped only when it is that very letter; otherwise the same thing would be
    // shown twice in an item six cells wide. An item registered with a function key keeps the
    // number even when it also has a letter: the bar reads as a sequence of numbers, and a gap in
    // it would be worse than a missing underline.
    const bool named = !keys().empty() && keys().front().letter() != 0;

    const std::string what = named && shortcut_column(whole) >= 0 ? std::string{} : tag();

    const int aside = measured(what).wide;

    if (aside > 0) made.put({-seen.at.x, -seen.at.y}, cut(what, given.wide), plain);

    const int room = given.wide - aside;

    if (room <= 0) return made;

    made.fill({{aside - seen.at.x, -seen.at.y}, {room, 1}}, label);

    if (text_.empty()) return made;

    const std::string shown = aside == 0 ? whole : elided(text_, room, Elide::right, dots);

    made.put({aside - seen.at.x, -seen.at.y}, shown, label);

    // The column is computed on this text, not on the earlier one: with the tag the name is
    // narrower, so a letter visible in the wider one may already be inside the ellipsis here. A
    // disabled item marks no letter, since its key does nothing.
    if (enabled())
        talcite::ui::marking(made, {aside - seen.at.x, -seen.at.y}, shown, shortcut_column(shown),
                          label, how.style(role() + ".key"));

    return made;
}

// A click does the same as the key and does not take the focus. The key bar in Midnight Commander
// does not take it from the panel, because after clicking "Copy" what was selected is to be
// copied, not whatever just got the focus.
bool KeyBarItem::handle(ClickEvent&) {
    pick();

    return true;
}

}
