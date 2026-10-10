#include "talcite/field.hpp"

#include <algorithm>
#include <utility>

#include "talcite/encoding.hpp"
#include "talcite/root.hpp"
#include "talcite/skin.hpp"


namespace talcite::ui {

namespace {

// One step back in UTF-8: we walk back over continuation bytes until we stand at the start of a
// character. Forward, `utf8::decode` does this; backward we have to do it ourselves, because the
// encoding reads in one direction only.
std::size_t before(const std::string& text, std::size_t at) {
    if (at == 0) return 0;

    --at;

    while (at > 0 && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80) --at;

    return at;
}

// One character forward from byte offset `at`; the end of the text when there is nothing left to
// decode.
std::size_t after(const std::string& text, std::size_t at) {
    if (at >= text.size()) return text.size();

    const std::size_t began = at;

    (void)utf8::decode(text, at);

    return at > began ? at : text.size();
}

}

// How many cells a piece of the text takes. We count widths, not bytes and not characters;
// otherwise the first double-width character would throw off both the cursor and the scrolling. A
// concealed field draws every character as one dot, so there a cell is a character; the width would
// reveal which characters were typed.
int Field::spanned(std::size_t from, std::size_t upto) const {
    int made = 0;

    std::size_t at = from;

    while (at < upto && at < text_.size()) {
        const std::size_t began = at;
        const char32_t one = utf8::decode(text_, at);

        made += concealed_ ? 1 : width(one);

        if (began == at) break;
    }

    return made;
}

// A single-line input field `wide` cells wide, with the cursor placed after the initial text.
Field::Field(int wide, std::string text) : text_(std::move(text)), wide_(wide) {
    role("talcite.input");

    at_ = text_.size();
}

// A field holds no children.
int Field::capacity() const { return 0; }


// Asks for its configured width and one row, within the room offered.
Size Field::wants(Size room) const {
    return {std::min(std::max(wide_, 0), room.wide), std::min(1, room.high)};
}

// The field is the only widget that has something to say about the cursor, and it says it in its
// own coordinates, without looking at the visible part: clipping is the root's job. The answer is
// true only after the frame is composed, because `from_` (how far the content is scrolled) is
// computed in `paint`, once the width the field really got is known.
std::optional<Spot> Field::caret() const {
    if (!focused()) return {};

    return Spot{spanned(from_, at_), 0};
}

// The cursor position in characters, counted from the start of the text.
std::size_t Field::cursor() const {
    std::size_t made = 0;
    std::size_t at = 0;

    while (at < at_ && at < text_.size()) {
        const std::size_t began = at;

        (void)utf8::decode(text_, at);

        if (began == at) break;

        ++made;
    }

    return made;
}

// Outside, the cursor counts in characters; inside, in bytes. The caller has no reason to know
// about UTF-8 or about cells; both measures are needed here, but they belong to the field.
void Field::cursor(std::size_t given) {
    at_ = 0;

    for (std::size_t which = 0; which < given && at_ < text_.size(); ++which)
        at_ = after(text_, at_);

    guessing();
}

// Replaces the content, puts the cursor at its end and scrolls back to the start.
void Field::text(std::string given) {
    text_ = std::move(given);
    at_ = text_.size();
    from_ = 0;

    guessing();
}

// Sets the completion source: given the current text, it returns the full suggested text.
void Field::guess(std::function<std::string(const std::string& what)> given) {
    guess_ = std::move(given);

    guessing();
}

// The suggestion is computed when the content changes, not when painting. The caller may look it up
// in a directory or in a history, which can be costly, while painting happens on every frame and is
// no place for such work.
//
// We suggest only when the cursor stands at the end: in the middle of the text, an addition after
// the cursor would lie about what a key press will do. A suggestion that does not start with what
// has already been typed is rejected, because it cannot be appended.
//
// A concealed field does not suggest at all: a suggestion shows text openly, and its source (a
// history, a directory) could reveal an old secret or its beginning.
void Field::guessing() {
    ahead_.clear();

    if (!guess_ || concealed_ || at_ != text_.size()) return;

    const std::string whole = guess_(text_);

    if (whole.size() <= text_.size()) return;

    if (whole.compare(0, text_.size(), text_) != 0) return;

    ahead_ = whole.substr(text_.size());
}

// What may be typed. A numeric field is one lambda, not a separate widget: Midnight Commander has
// no numeric widget and enters numbers in an ordinary input, so there is nothing to imitate. The
// same hook serves to forbid slashes in a file name or to allow only hexadecimal digits.
void Field::allows(std::function<bool(char32_t one)> given) { allows_ = std::move(given); }

// A concealed field, for keys and passwords: every character is drawn as a dot and there are no
// suggestions. The content stays the same and `text()` returns it in the clear; only the screen is
// hidden. The field has no copying anyway, so there is nothing to switch off.
void Field::concealed(bool on) {
    concealed_ = on;

    guessing();
}

// Inserts text at the cursor, as typed or pasted: line breaks become spaces and characters the
// filter refuses are dropped one by one.
void Field::insert(const std::string& what) {
    std::string clean;

    clean.reserve(what.size());

    std::size_t at = 0;

    while (at < what.size()) {
        const std::size_t began = at;
        const char32_t one = utf8::decode(what, at);

        if (began == at) break;

        // The field has a single line, so line breaks from a paste become spaces instead of letting
        // into the text a character we cannot draw.
        const char32_t which = (one == U'\n' || one == U'\r') ? U' ' : one;

        // The filter works both when typing and when pasting: "12ab34" pasted into a digits-only
        // field leaves "1234" instead of rejecting the whole. Rejecting everything would punish one
        // bad character in the middle, and letting it through would lie about what the field
        // accepts.
        if (allows_ && !allows_(which)) continue;

        utf8::encode(which, clean);
    }

    if (clean.empty()) return;

    text_.insert(at_, clean);
    at_ += clean.size();

    guessing();
}

// Draws the visible part of the content (or dots, when concealed) and the dimmed suggestion after
// it.
View Field::paint(Size given, Area seen) {
    View made{seen.size};

    // Transparent where nothing is drawn; the same rule and the same reason as in the button.
    made.clear({{0, 0}, seen.size});

    const int room = std::max(given.wide, 0);

    // The scroll offset is computed here, because only here is the width the field really got
    // known; the parent may give less than was asked for. This is the only place where painting
    // changes state, and it does so on purpose: otherwise the cursor could move out of the visible
    // part.
    if (at_ < from_) from_ = at_;

    while (room > 0 && spanned(from_, at_) >= room) from_ = after(text_, from_);

    const Skin& how = skin();

    // The underline comes from the built-in skin, in the `talcite.input` role. In Midnight
    // Commander a field is marked by its background color alone, so once colors arrive it is enough
    // to replace the skin entry; not a single line of code is added or removed.
    const Style look = how.style(enabled() ? role() : role() + ".disabled");

    made.fill({{-seen.at.x, -seen.at.y}, {room, 1}}, look);

    // The visible part of the content, clipped to the width.
    std::size_t upto = from_;
    int wide = 0;

    while (upto < text_.size()) {
        const std::size_t began = upto;
        const std::size_t next = after(text_, upto);
        const int much = spanned(began, next);

        if (wide + much > room) break;

        wide += much;
        upto = next;

        if (began == upto) break;
    }

    if (concealed_) {
        const std::string dot = utf8::encode(how.glyph("talcite.input.conceal"));

        std::string dots;

        for (int which = 0; which < wide; ++which) dots += dot;

        made.put({-seen.at.x, -seen.at.y}, dots, look);
    } else {
        made.put({-seen.at.x, -seen.at.y}, text_.substr(from_, upto - from_), look);
    }

    // The suggestion is dimmed, after the content, as far as room allows. It is not part of the
    // text and does not enter `text()` until someone accepts it.
    if (!ahead_.empty() && wide < room) {
        const Style dim = Style::merge(look, how.style(role() + ".guess"));

        made.put({wide - seen.at.x, -seen.at.y}, ahead_, dim);
    }

    return made;
}

// Enter is announced by the field as a `SubmitEvent` ("entry confirmed") and the field does nothing
// more: it knows neither the window nor its buttons. Whoever wants Enter in a field to confirm the
// form hangs a handler on the form (the event bubbles up, so one handler covers every field) and
// presses its own OK there. The key is taken only when someone took the event; otherwise it travels
// on.
//
// Esc and function keys are passed on, which is how Esc in a field closes the dialog, as in
// Midnight Commander. Ordinary letters are always taken: otherwise a "t" typed in a field would
// press a button that has that letter among its hotkeys.
//
// Tab is never taken, so it always moves focus to the next widget.
bool Field::handle(KeyEvent& what) {
    if (!enabled()) return false;

    if (what.mods().ctrl || what.mods().alt) return false;

    switch (what.key().code()) {
        // Tab is not taken here. A navigation key must not disappear depending on the field's
        // content, so completing the suggestion stays under the right arrow, which moves within the
        // field's own content and takes nothing away.

        case Code::right:
            // At the end of the content, the right arrow takes one letter of the suggestion.
            if (at_ == text_.size() && !ahead_.empty()) {
                const std::size_t much = after(ahead_, 0);

                text_ += ahead_.substr(0, much);
                at_ = text_.size();

                guessing();

                return true;
            }

            at_ = after(text_, at_);

            guessing();

            return true;

        case Code::left: at_ = before(text_, at_); guessing(); return true;

        case Code::enter: return trigger(SubmitEvent{});

        case Code::home: at_ = 0; guessing(); return true;

        case Code::end: at_ = text_.size(); guessing(); return true;

        case Code::backspace: {
            if (at_ == 0) return true;

            const std::size_t began = before(text_, at_);

            text_.erase(began, at_ - began);
            at_ = began;

            guessing();

            return true;
        }

        case Code::remove: {
            if (at_ >= text_.size()) return true;

            text_.erase(at_, after(text_, at_) - at_);

            guessing();

            return true;
        }

        default: break;
    }

    const char32_t one = what.key().letter();

    if (one == 0) return false;

    std::string said;

    utf8::encode(one, said);

    // A filtered-out character is swallowed, not passed up. The key was meant for the field, and
    // the field not wanting it does not mean someone else should get it; otherwise a "t" rejected
    // in a digits-only field would press the "Yes" button in a dialog.
    insert(said);

    return true;
}

// A paste goes into the text through the same filter as typing.
bool Field::handle(PasteEvent& what) {
    if (!enabled()) return false;

    insert(what.text());

    return true;
}

// A click puts the cursor where it landed and takes focus. The column is converted with the same
// measure that `paint` uses; otherwise the cursor would land next to the spot that was aimed at.
bool Field::handle(ClickEvent& what) {
    if (!enabled()) return true;

    if (root() != nullptr) focus();

    const int wanted = local(what.where()).x;

    std::size_t at = from_;
    int wide = 0;

    while (at < text_.size()) {
        const std::size_t next = after(text_, at);
        const int much = spanned(at, next);

        if (wide + much > wanted) break;

        // Stop when there is no progress; the same guard as in every other walk over the text.
        if (next == at) break;

        wide += much;
        at = next;
    }

    at_ = at;

    guessing();

    return true;
}

}
