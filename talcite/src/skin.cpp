#include "talcite/skin.hpp"

#include <optional>
#include <type_traits>
#include <utility>

#include "lining.hpp"

namespace talcite::ui {

namespace {

// Faint text: the built-in mark of anything disabled or secondary.
Style faded() {
    Style made{};

    made.faint = true;

    return made;
}

// Reverse video: the built-in mark of focus and selection.
Style turned() {
    Style made{};

    made.reverse = true;

    return made;
}

// Underlined text: the built-in mark of hot keys and input fields.
Style lined() {
    Style made{};

    made.under = true;

    return made;
}

// Bold as the mark of a marked item. In Midnight Commander a marked file is yellow, but the built-in
// skin has no colors, and bold shows even on a monochrome terminal.
Style bolded() {
    Style made{};

    made.bold = true;

    return made;
}

// Bold and reversed: a marked item under the cursor.
Style boldly() {
    Style made{};

    made.bold = true;
    made.reverse = true;

    return made;
}

// Reversed and underlined: the current item among several reversed ones.
Style pointed() {
    Style made{};

    made.reverse = true;
    made.under = true;

    return made;
}

// The built-in style table. It reproduces the colorless look cell for cell: reverse stays reverse,
// faint stays faint, so the screens the tests expect do not depend on the skin.
//
// Two entries are placeholders waiting for colors, and they are meant to disappear from here, not
// from the code: talcite.input carries an underline, because in Midnight Commander a field is set
// apart by its background color alone, and talcite.list.mark is empty, because an inactive MC panel
// shows no cursor (in a dialog it shows it in another color, which we do not have yet).
//
// All of the library's names start with "talcite.", and that is not decoration. The key is a name,
// so an application may invent any; without the prefix its own "input" would cover the library's
// role and nobody would know. Other names start with the program's name.
Styles building() {
    return {
        {"talcite.normal", Style{}},

        {"talcite.frame", Style{}},
        {"talcite.frame.title", Style{}},
        {"talcite.title", Style{}},

        // The paper under image cells that are only partly covered: the edge of a cut-out shape.
        // The built-in skin leaves it transparent, so the parent shows through.
        {"talcite.image", Style{}},

        // The text viewer draws only the content, so the built-in skin leaves it plain, the same as
        // an ordinary label.
        {"talcite.viewer", Style{}},

        // Matches in the viewer: the built-in skin reverses them, because there are no colors yet
        // and reverse shows on every terminal. The current one (where the jump stands) has its own
        // role, so it can be told apart from the rest; the built-in skin adds an underline, because
        // without colors there is no other distinction.
        {"talcite.viewer.mark", turned()},
        {"talcite.viewer.mark.focus", pointed()},

        // The editor draws the same content as the viewer, so the built-in skin does not set it
        // apart. The selection is reversed, like everything that has to be visible without colors.
        {"talcite.editor", Style{}},
        {"talcite.editor.select", turned()},

        {"talcite.input", lined()},
        {"talcite.input.guess", faded()},
        {"talcite.input.disabled", faded()},

        {"talcite.button", Style{}},
        {"talcite.button.focus", turned()},
        {"talcite.button.key", lined()},
        {"talcite.button.disabled", faded()},

        // A message has its own family of roles, not the same as the rest of the window: an error
        // box looks different from an ordinary window, and one role cannot be red and navy at once.
        // The buttons in a message repeat the values of ordinary ones, so the built-in skin shows
        // them the same; only a color skin makes the difference.
        {"talcite.message", Style{}},
        {"talcite.message.title", Style{}},
        {"talcite.message.text", Style{}},
        {"talcite.message.box", Style{}},
        {"talcite.message.button", Style{}},
        {"talcite.message.button.focus", turned()},
        {"talcite.message.button.key", lined()},
        {"talcite.message.button.disabled", faded()},

        // An error is the same widget with a different stem, so its family of roles is the same
        // too. Without colors it differs in nothing, and that is intended: Midnight Commander's mono
        // skin does not tell an error box from an ordinary window either. The palette makes the
        // difference, not the code.
        {"talcite.error", Style{}},
        {"talcite.error.title", Style{}},
        {"talcite.error.text", Style{}},
        {"talcite.error.box", Style{}},
        {"talcite.error.button", Style{}},
        {"talcite.error.button.focus", turned()},
        {"talcite.error.button.key", lined()},
        {"talcite.error.button.disabled", faded()},

        {"talcite.list", Style{}},
        {"talcite.list.focus", turned()},
        {"talcite.list.mark", Style{}},
        {"talcite.list.disabled", faded()},

        {"talcite.tick", Style{}},
        {"talcite.tick.focus", turned()},
        {"talcite.tick.key", lined()},
        {"talcite.tick.disabled", faded()},

        {"talcite.choice", Style{}},
        {"talcite.choice.focus", turned()},
        {"talcite.choice.disabled", faded()},

        {"talcite.select", Style{}},
        {"talcite.select.focus", turned()},
        {"talcite.select.disabled", faded()},

        {"talcite.table", Style{}},
        {"talcite.table.head", turned()},
        {"talcite.table.focus", turned()},
        {"talcite.table.mark", bolded()},
        {"talcite.table.mark.focus", boldly()},

        // The diff view. Without colors the two sides look the same (the palette makes the
        // difference), but the marked parts of a line must show even on a monochrome terminal:
        // removed parts are underlined, added parts bold. The cursor is reversed, as in the table.
        {"talcite.diff", Style{}},
        {"talcite.diff.head", turned()},
        {"talcite.diff.title", bolded()},
        {"talcite.diff.focus", turned()},
        {"talcite.diff.gone", Style{}},
        {"talcite.diff.came", Style{}},
        {"talcite.diff.gone.mark", lined()},
        {"talcite.diff.came.mark", bolded()},
        {"talcite.diff.number", faded()},

        {"talcite.menubar", Style{}},
        {"talcite.menubar.focus", turned()},
        {"talcite.menubar.key", lined()},

        {"talcite.menu", Style{}},
        {"talcite.menu.focus", turned()},
        {"talcite.menu.disabled", faded()},
        {"talcite.menu.key", lined()},
        {"talcite.menu.frame", Style{}},

        {"talcite.shade", faded()},

        {"talcite.legend.tag", Style{}},
        {"talcite.legend.key", lined()},
        {"talcite.legend.text", turned()},
        {"talcite.legend.disabled", faded()},

        {"talcite.scroll", Style{}},
        {"talcite.scroll.thumb", Style{}},

        {"talcite.gauge", Style{}},
        {"talcite.gauge.done", Style{}},
        {"talcite.gauge.text", Style{}},
    };
}

// Adds the eight frame glyphs of `how` under the names `named`.across, .upright, .upper.left and so
// on. Frame glyphs are stored one by one, as in a Midnight Commander skin file; the set is put
// together from them only in the constructor.
void ruled(Glyphs& into, std::string_view named, Border how) {
    const Lines one = drawn(how);
    const std::string prefix{named};

    into.emplace(prefix + ".across", one.across);
    into.emplace(prefix + ".upright", one.upright);
    into.emplace(prefix + ".upper.left", one.upper_left);
    into.emplace(prefix + ".upper.right", one.upper_right);
    into.emplace(prefix + ".lower.left", one.lower_left);
    into.emplace(prefix + ".lower.right", one.lower_right);
    into.emplace(prefix + ".left.joint", one.left_joint);
    into.emplace(prefix + ".right.joint", one.right_joint);
}

// The built-in glyph table, the counterpart of `building()`, which assembles the styles.
Glyphs glyphing() {
    Glyphs made{
        {"talcite.scroll.up", U'▲'},   {"talcite.scroll.down", U'▼'}, {"talcite.scroll.left", U'◀'},
        {"talcite.scroll.right", U'▶'}, {"talcite.scroll.thumb", U'█'}, {"talcite.scroll.track", U'░'},

        {"talcite.gauge.done", U'█'},  {"talcite.gauge.rest", U'░'},  {"talcite.gauge.left", U'['},
        {"talcite.gauge.right", U']'},

        {"talcite.choice.dot", U'•'},
        {"talcite.input.conceal", U'•'},
        {"talcite.select.mark", U'▼'},

        {"talcite.elide", U'…'},
    };

    ruled(made, "talcite.line.thin", Border::thin);
    ruled(made, "talcite.line.twin", Border::twin);
    ruled(made, "talcite.line.thick", Border::thick);

    return made;
}

// The glyph for the level (`unicode` or not): from the skin, else from the built-in table `spare`,
// else '?', never zero (the reason is at `Skin::style`).
char32_t glyphed(const Glyphs& among, const Glyphs& spare, const std::string& named,
                 bool unicode) {
    if (const auto at = among.find(named); at != among.end())
        if (const std::optional<char32_t> one = at->second.fitted(unicode)) return *one;

    if (const auto at = spare.find(named); at != spare.end())
        if (const std::optional<char32_t> one = at->second.fitted(unicode)) return *one;

    return U'?';
}

// The frame set `named` for the level. It is assembled at construction and after each change of the
// glyph level: `Lines` is asked for eight times per frame and must not be assembled at every paint.
Lines gathered(const Glyphs& among, const Glyphs& spare, std::string_view named, bool unicode) {
    const std::string prefix{named};

    return {glyphed(among, spare, prefix + ".upper.left", unicode),
            glyphed(among, spare, prefix + ".upper.right", unicode),
            glyphed(among, spare, prefix + ".lower.left", unicode),
            glyphed(among, spare, prefix + ".lower.right", unicode),
            glyphed(among, spare, prefix + ".across", unicode),
            glyphed(among, spare, prefix + ".upright", unicode),
            glyphed(among, spare, prefix + ".left.joint", unicode),
            glyphed(among, spare, prefix + ".right.joint", unicode)};
}

// The built-in glyph table, built once; frame sets fall back to it when the skin has no glyph at the
// current level.
const Glyphs& glyphed() {
    static const Glyphs one = glyphing();

    return one;
}

}

// A skin is complete from birth. The constructor starts from the built-in values and overwrites
// them with what the caller gave: the built-in table plus the caller's entries, merged once rather
// than at every lookup. Built-in values come as one value for every level, so a library role always
// has something to show; a role given from outside replaces the built-in one entirely, and which
// level it lacks is settled only at lookup, because only then is the terminal known.
Skin::Skin(Styles styles, Glyphs glyphs) : styles_(building()), glyphs_(glyphing()) {
    for (auto& [named, one] : styles) styles_.insert_or_assign(named, one);

    for (const auto& [named, one] : glyphs) glyphs_.insert_or_assign(named, one);

    framed();
}

Skin::Skin() : Skin(Styles{}, Glyphs{}) {}

// The built-in skin, one instance for the whole program, because `Node::skin()` without a root has
// to return a reference, not a value. A function-local static is built at the first call and never
// again.
const Skin& Skin::plain() {
    static const Skin one;

    return one;
}

// The style of a role. A lookup never throws, because it is called from painting. A typo in a role
// name, or a role an application invented and never set, would crash the program in the middle of
// composing a frame, and not where the mistake is but at the first repaint.
//
// The level is settled here, not at construction: the skin carries variants for every level, and
// the root tells it which terminal it is on through `colors` and `unicode`. When there is no
// variant at this level, the built-in one for the same role is used, without guessing and without
// approximating colors. A skin with only `rgb` on an `ansi` terminal therefore shows what the
// built-in skin shows.
//
// An unknown glyph gives '?', never zero: in `View` zero means the continuation of a double-width
// character, and composing a frame skips such cells, so zero would leave a hole in the picture
// instead of a visible error.
Style Skin::style(std::string_view key) const {
    if (const auto at = styles_.find(key); at != styles_.end())
        if (const std::optional<Style> one = at->second.fitted(colors_)) return *one;

    if (this == &plain()) return Style{};

    return plain().style(key);
}

// The glyph of a role at the current level, with the same fallbacks as `style`.
char32_t Skin::glyph(std::string_view key) const {
    return glyphed(glyphs_, glyphed(), std::string{key}, unicode_);
}

// The frame set for a border style, ready-made at construction and after each change of the glyph
// level (`framed`). `Border::none` gives the thin set: a widget without a frame does not ask for
// glyphs, and if it did, it gets a set that can be drawn rather than spaces pretending to be none.
Lines Skin::lines(Border border) const { return lines_[static_cast<std::size_t>(border)]; }

// Sets the color level of the terminal the skin is shown on.
void Skin::colors(Colors given) { colors_ = given; }

// Sets whether the terminal shows Unicode, and reassembles the frame sets for it.
void Skin::unicode(bool given) {
    unicode_ = given;

    framed();
}

// Assembles the frame sets of all border styles for the current glyph level.
void Skin::framed() {
    for (const Border one : {Border::none, Border::thin, Border::twin, Border::thick})
        lines_[static_cast<std::size_t>(one)] = gathered(glyphs_, glyphed(), named(one), unicode_);
}

// The variant at the highest level not above `level`. A skin with only `rgb` on an `ansi` terminal
// has nothing to show, and returns nothing rather than an approximation: what to draw then is
// decided by `Skin::style`, which falls back to the built-in skin.
std::optional<Style> Shades::fitted(Colors level) const {
    const std::array<const std::optional<Style>*, 4> by{&mono, &ansi, &palette, &rgb};

    for (int at = static_cast<int>(level); at >= 0; --at)
        if (by[static_cast<std::size_t>(at)]->has_value()) return *by[static_cast<std::size_t>(at)];

    return std::nullopt;
}

Shades::Shades(Style same) : mono(same), ansi(same), palette(same), rgb(same) {}

// A Unicode terminal takes the Unicode glyph, or the ASCII one when there is none; a terminal
// without Unicode takes only ASCII, because anything outside it would come out as garbage.
std::optional<char32_t> Signs::fitted(bool wanted) const {
    if (wanted && unicode.has_value()) return unicode;

    return ascii;
}

Signs::Signs(char32_t same) : ascii(same), unicode(same) {}

}
