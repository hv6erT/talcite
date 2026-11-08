#include "tal/testing.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "talcite/button.hpp"
#include "talcite/capabilities.hpp"
#include "talcite/encoding.hpp"
#include "talcite/frame.hpp"
#include "talcite/line.hpp"
#include "talcite/list.hpp"
#include "talcite/listitem.hpp"
#include "talcite/root.hpp"
#include "talcite/span.hpp"
#include "talcite/style.hpp"
#include "talcite/skin.hpp"

using namespace talcite;

namespace {

// A node that exposes what a widget receives in `paint`.
class Peek : public ui::Node {
public:
    // The skin this node sees, as `paint` would get it.
    [[nodiscard]] const ui::Skin& seen() const { return skin(); }
};

// A container that lays its children out, just enough for a list and a focusable leaf to stand side
// by side.
class Hold : public ui::Node {
public:
    // Takes any number of children.
    [[nodiscard]] int capacity() const override { return ui::plenty; }

    // Wants all the room it is given.
    [[nodiscard]] ui::Size wants(ui::Size room) const override { return room; }

protected:
    // Lays out only the first child. The rest are here so that focus can be given to them, and they
    // get a rectangle of zero size so that they paint over nothing.
    void layout(ui::Size given) override {
        bool first = true;

        for (const std::shared_ptr<ui::Node>& one : children()) {
            one->arrange({{0, 0}, first ? given : ui::Size{}});

            first = false;
        }
    }

    // Paints the children over a background of spaces.
    void paint(Canvas& onto) const override {
        onto.fill({{0, 0}, onto.size()}, Style{});

        for (const std::shared_ptr<ui::Node>& one : children()) one->render(onto);
    }
};

// A leaf that accepts focus, used to take focus away from the widget under test.
class Perch : public ui::Node {
public:
    // Always accepts focus.
    [[nodiscard]] bool focusable() const override { return true; }
};

// Reads row `y` of a drawn view as UTF-8 text, skipping the empty halves of wide cells and
// dropping trailing spaces.
std::string read(const Snapshot& one, int y) {
    std::string made;

    for (int x = 0; x < one.size().wide; ++x) {
        const char32_t letter = one.at({x, y}).letter;

        if (letter != 0) made += talcite::spelled(letter);
    }

    while (!made.empty() && made.back() == ' ') made.pop_back();

    return made;
}

// A style with only the foreground set to the given RGB color.
Style tinted(std::uint8_t red, std::uint8_t green, std::uint8_t blue) {
    Style made{};

    made.foreground = Color{false, red, green, blue};

    return made;
}

}

TRIAL("Skin: the built-in one knows the roles and glyphs") {
    const ui::Skin one;

    OUGHT(one.style("talcite.list.focus").reverse);
    OUGHT(one.style("talcite.input").under);
    OUGHT(one.style("talcite.shade").faint);

    SAME(one.glyph("talcite.line.thin.across"), U'─');
    SAME(one.glyph("talcite.line.twin.across"), U'═');
    SAME(one.glyph("talcite.scroll.thumb"), U'█');
}

// Reading never throws, because it is called from painting. An unknown role gives the terminal's
// default colors, and an unknown glyph gives a question mark.
TRIAL("Skin: an unknown role gives an empty style, an unknown glyph a question mark") {
    const ui::Skin one;

    SAME(one.style("czego.tu.nie.ma"), Style{});
    SAME(one.glyph("czego.tu.nie.ma"), U'?');
}

// Zero is forbidden: in a `Sheet` it marks the continuation of a two-column character, and
// composing a frame skips such cells, so a zero would leave a hole in the picture instead of a
// visible error.
TRIAL("Skin: an unknown glyph is never zero") {
    const ui::Skin one;

    OUGHT(one.glyph("nie.ma.takiego") != 0);
    OUGHT(ui::Skin::plain().glyph("nie.ma.takiego") != 0);
}

// This is `Store(spare, held)`: the caller gives its own entries and the library adds the rest, only
// merged once, at construction, not on every read.
TRIAL("Skin: given entries override, the rest stays built in") {
    const ui::Skin one{{{"talcite.list.focus", tinted(10, 20, 30)}}};

    SAME(one.style("talcite.list.focus"), tinted(10, 20, 30));

    // Untouched roles are still as in the built-in skin.
    OUGHT(one.style("talcite.input").under);
    OUGHT(one.style("talcite.shade").faint);
    SAME(one.glyph("talcite.line.thin.across"), U'─');
}

TRIAL("Skin: plain always gives the built-in skin, also when overridden") {
    const ui::Skin one{{{"talcite.list.focus", tinted(10, 20, 30)}}};

    SAME(one.style("talcite.list.focus"), tinted(10, 20, 30));
    OUGHT(ui::Skin::plain().style("talcite.list.focus").reverse);
    OUGHT(ui::Skin::plain().style("talcite.list.focus").foreground.transparent);

    SAME(ui::Skin::plain().glyph("talcite.line.thin.across"), U'─');
}

TRIAL("Skin: a border set is made of single glyphs") {
    const ui::Skin one;

    const ui::Lines thin = one.lines(ui::Border::thin);

    SAME(thin.across, U'─');
    SAME(thin.upright, U'│');
    SAME(thin.upper_left, U'┌');
    SAME(thin.right_joint, U'┤');

    const ui::Lines twin = one.lines(ui::Border::twin);

    SAME(twin.across, U'═');
    SAME(twin.lower_right, U'╝');

    // Without a border it gives the thin set, the same choice `drawn` makes for `Border::none`.
    SAME(one.lines(ui::Border::none).across, U'─');
    SAME(one.lines(ui::Border::none).upper_left, U'┌');

    SAME(one.lines(ui::Border::thick).across, U'━');
}

// The set is assembled, not stored separately: replacing one glyph changes the whole set of eight.
// That is correct by definition, because a skin cannot change after it is built.
TRIAL("Skin: replacing a glyph changes the assembled set") {
    const ui::Skin one{{}, {{"talcite.line.thin.across", U'='}}};

    SAME(one.glyph("talcite.line.thin.across"), U'=');
    SAME(one.lines(ui::Border::thin).across, U'=');

    // The rest of the set is unchanged.
    SAME(one.lines(ui::Border::thin).upright, U'│');

    // And the heavier sets are not touched.
    SAME(one.lines(ui::Border::twin).across, U'═');
}

TRIAL("Skin: the built-in skin is always the same instance") {
    SAME(&ui::Skin::plain(), &ui::Skin::plain());
}

TRIAL("Skin: a node without a root gets the built-in skin") {
    const Peek one;

    SAME(&one.seen(), &ui::Skin::plain());
    OUGHT(one.seen().style("talcite.list.focus").reverse);
}

TRIAL("Skin: a node in a tree gets the root's skin") {
    ui::Root top;

    const auto one = std::make_shared<Peek>();
    const auto deeper = std::make_shared<Peek>();

    one->children().add(deeper);
    top.children().add(one);

    SAME(&one->seen(), &top.skin());
    SAME(&deeper->seen(), &top.skin());

    // The built-in skin is a different instance; the root has its own.
    OUGHT(&top.skin() != &ui::Skin::plain());
}

// Replacing the whole palette is the entire change of look on the fly: there is no notification and
// no walk over the tree, because a node reads the skin only when it paints.
TRIAL("Skin: a replacement on the root is visible deep down at once") {
    ui::Root top;

    const auto one = std::make_shared<Peek>();
    const auto deeper = std::make_shared<Peek>();

    one->children().add(deeper);
    top.children().add(one);

    OUGHT(deeper->seen().style("talcite.list.focus").reverse);

    top.skin(ui::Skin{{{"talcite.list.focus", tinted(1, 2, 3)}}});

    SAME(deeper->seen().style("talcite.list.focus"), tinted(1, 2, 3));

    // The rest still comes from the built-in skin, because a new skin is also complete from birth.
    OUGHT(deeper->seen().style("talcite.input").under);
}

// A node removed from the tree stops seeing the root's skin, and is not left with nothing.
TRIAL("Skin: a removed node goes back to the built-in skin") {
    ui::Root top;

    const auto one = std::make_shared<Peek>();

    top.children().add(one);
    top.skin(ui::Skin{{{"talcite.input", tinted(9, 9, 9)}}});

    SAME(one->seen().style("talcite.input"), tinted(9, 9, 9));

    (void)top.children().remove(one.get());

    SAME(&one->seen(), &ui::Skin::plain());
    OUGHT(one->seen().style("talcite.input").under);
}

// The skin is not closed. An application invents its own roles beside the ones the library knows,
// and reads them the same way, because the key is a name and not an enumeration that cannot be
// extended from outside. The file panel of a file manager needs this for directories, executable
// files and selected entries.
TRIAL("Skin: an application adds its own roles beside the built-in ones") {
    ui::Root top;

    const auto one = std::make_shared<Peek>();

    top.children().add(one);

    top.skin(ui::Skin{{
        {"fork.panel.directory", tinted(1, 2, 3)},
        {"fork.panel.exec", tinted(4, 5, 6)},
    }});

    SAME(one->seen().style("fork.panel.directory"), tinted(1, 2, 3));
    SAME(one->seen().style("fork.panel.exec"), tinted(4, 5, 6));

    // The library's roles stand beside them, untouched.
    OUGHT(one->seen().style("talcite.list.focus").reverse);
    OUGHT(one->seen().style("talcite.input").under);
}

TRIAL("Skin: custom glyphs are added the same way as custom roles") {
    const ui::Skin one{{}, {{"fork.panel.mark", U'*'}}};

    SAME(one.glyph("fork.panel.mark"), U'*');
    SAME(one.glyph("talcite.line.thin.across"), U'─');
}

// A role nobody set gives the terminal's default colors, and that cannot be told apart from a role
// set to the defaults on purpose. A widget from outside the library that wants its own fallback look
// has to seed it into the skin itself, when the skin is built.
TRIAL("Skin: a foreign role without an entry gives the terminal defaults") {
    const ui::Skin one;

    SAME(one.style("fork.panel.directory"), Style{});
    SAME(ui::Skin::plain().style("fork.panel.directory"), Style{});
}

// The upper style adds but does not take away. That is what merging is for: a widget from outside
// the library keeps its own fallback look and lays over it what the skin said; when the skin said
// nothing, its own look stays.
TRIAL("Style: merging adds the upper color and combines the flags") {
    Style base{};

    base.foreground = Color{false, 1, 2, 3};
    base.under = true;

    Style over{};

    over.background = Color{false, 9, 9, 9};
    over.reverse = true;

    const Style made = Style::merge(base, over);

    // The lower foreground stayed, because the upper style did not give one; the background came
    // from the upper style.
    SAME(made.foreground, (Color{false, 1, 2, 3}));
    SAME(made.background, (Color{false, 9, 9, 9}));

    OUGHT(made.under);
    OUGHT(made.reverse);
    OUGHT(!made.bold);
}

TRIAL("Style: a color set in the upper style hides the lower one") {
    Style base{};

    base.foreground = Color{false, 1, 2, 3};

    Style over{};

    over.foreground = Color{false, 7, 7, 7};

    SAME(Style::merge(base, over).foreground, (Color{false, 7, 7, 7}));
}

// An empty style changes nothing, and this is the case merging exists for: a role the skin has not
// heard of gives exactly an empty style.
TRIAL("Style: merging with an empty style keeps the fallback look") {
    Style base{};

    base.foreground = Color{false, 1, 2, 3};
    base.bold = true;
    base.under = true;

    SAME(Style::merge(base, Style{}), base);

    // The same the other way round: an empty lower style gives the upper one unchanged.
    SAME(Style::merge(Style{}, base), base);
}

// The cost of the rule, stated plainly: the upper style cannot turn off a flag of the lower one,
// because `Style` has no "not set" state for flags; only colors have it.
TRIAL("Style: the upper style does not turn off a lower flag") {
    Style base{};

    base.under = true;

    OUGHT(Style::merge(base, Style{}).under);
}

// This is how it is used: a widget from outside the library with its own fallback look.
TRIAL("Skin: a foreign widget lays the skin over its own fallback look") {
    Style mine{};

    mine.bold = true;

    const ui::Skin bare;

    // The skin does not know this role, so the widget's own look stays.
    SAME(Style::merge(mine, bare.style("fork.panel.directory")), mine);

    const ui::Skin told{{{"fork.panel.directory", tinted(1, 2, 3)}}};

    const Style made = Style::merge(mine, told.style("fork.panel.directory"));

    SAME(made.foreground, (Color{false, 1, 2, 3}));
    OUGHT(made.bold);
}

// The current row without focus has a role of its own, which the built-in skin leaves empty. It
// therefore looks the same as a plain row marker, and a skin with colors can fill it in without
// changing a line of code.
TRIAL("Skin: the current row without focus takes the list.mark role") {
    ui::Root top;

    const auto list = std::make_shared<ui::List>(20, 3);
    const auto first = std::make_shared<ui::ListItem>("jeden");
    const auto other = std::make_shared<ui::ListItem>("dwa");
    const auto perch = std::make_shared<Perch>();

    const auto nest = std::make_shared<Hold>();

    list->children().add(first);
    list->children().add(other);

    nest->children().add(list);
    nest->children().add(perch);
    top.children().add(nest);

    Style told{};

    told.foreground = Color{false, 5, 6, 7};

    top.skin(ui::Skin{{{"talcite.list.mark", told}}});

    list->focus();

    (void)top.render({{0, 0}, {20, 3}});

    // With focus the current row is in reverse video, not in the "without focus" role.
    SAME(list->which(), std::optional<std::size_t>{0});
    OUGHT(top.render({{0, 0}, {20, 3}}).at({0, 0}).style.reverse);

    // Once focus leaves, only the marker remains, and the role applies.
    perch->focus();

    const Snapshot made = top.render({{0, 0}, {20, 3}});

    SAME(list->which(), std::optional<std::size_t>{0});
    OUGHT(!made.at({0, 0}).style.reverse);
    SAME(made.at({0, 0}).style.foreground.red, static_cast<std::uint8_t>(5));

    // A row that is not current does not get the role.
    OUGHT(list->which() != std::optional<std::size_t>{1});
    OUGHT(made.at({0, 1}).style.foreground.transparent);
}

// Glyphs come from the skin too. The widget still chooses the weight, since that is a choice of
// shape, and the skin says which glyphs draw that weight.
TRIAL("Skin: a frame glyph from the skin goes onto the frame") {
    ui::Root top;

    const auto frame = std::make_shared<ui::Frame>("kot", ui::FrameShape{ui::Border::twin});

    top.children().add(frame);

    SAME(read(top.render({{0, 0}, {11, 3}}), 0), std::string{"╔══ kot ══╗"});

    top.skin(ui::Skin{{}, {{"talcite.line.twin.across", U'*'}}});

    SAME(read(top.render({{0, 0}, {11, 3}}), 0), std::string{"╔** kot **╗"});
}

// A terminal without box-drawing characters needs no separate branch in the code, only a second
// skin.
TRIAL("Skin: an ASCII skin draws a frame without box-drawing characters") {
    ui::Root top;

    const auto frame = std::make_shared<ui::Frame>("kot");

    top.children().add(frame);

    top.skin(ui::Skin{{},
                      {
                          {"talcite.line.thin.across", U'-'},
                          {"talcite.line.thin.upright", U'|'},
                          {"talcite.line.thin.upper.left", U'+'},
                          {"talcite.line.thin.upper.right", U'+'},
                          {"talcite.line.thin.lower.left", U'+'},
                          {"talcite.line.thin.lower.right", U'+'},
                      }});

    const Snapshot made = top.render({{0, 0}, {11, 3}});

    SAME(read(made, 0), std::string{"+-- kot --+"});
    SAME(read(made, 1), std::string{"|         |"});
    SAME(read(made, 2), std::string{"+---------+"});
}

TRIAL("Skin: the ellipsis comes from the skin") {
    ui::Root top;

    const auto one = std::make_shared<ui::Span>("dokumenty");

    one->elide(ui::Elide::right);

    top.children().add(one);

    SAME(read(top.render({{0, 0}, {5, 1}}), 0), std::string{"doku…"});

    top.skin(ui::Skin{{}, {{"talcite.elide", U'~'}}});

    SAME(read(top.render({{0, 0}, {5, 1}}), 0), std::string{"doku~"});
}

TRIAL("Skin: the scroller's track and thumb take their glyphs from the skin") {
    ui::Root top;

    const auto list = std::make_shared<ui::List>(ui::plenty, ui::plenty, ui::ScrollShape{true});

    for (int which = 0; which < 10; ++which)
        list->children().add(std::make_shared<ui::ListItem>("wiersz"));

    top.children().add(list);

    Snapshot made = top.render({{0, 0}, {8, 4}});

    SAME(made.at({7, 0}).letter, U'█');
    SAME(made.at({7, 3}).letter, U'│');

    top.skin(ui::Skin{{},
                      {
                          {"talcite.scroll.thumb", U'#'},
                          {"talcite.scroll.track.upright", U'.'},
                      }});

    made = top.render({{0, 0}, {8, 4}});

    SAME(made.at({7, 0}).letter, U'#');
    SAME(made.at({7, 1}).letter, U'.');
    SAME(made.at({7, 3}).letter, U'.');
}

// --- The role stem given to a widget -----------------------------------------------------------
//
// A single role for all text in the program cannot be red and navy at once, and the text in an
// error box has to be red. Whoever builds such a box knows what stands in it, so the stem of the
// role name is given to the widget from outside, and the widget itself adds the suffixes for its
// state.

namespace {

// A style with only the foreground set to the given RGB color.
Style inked(std::uint8_t red, std::uint8_t green, std::uint8_t blue) {
    Style made{};

    made.foreground = Color{false, red, green, blue};

    return made;
}

}

TRIAL("Role: a span asks for the stem it was given") {
    ui::Root top;

    top.skin(ui::Skin{{{"talcite.normal", inked(1, 1, 1)}, {"moja.rola", inked(9, 9, 9)}}});

    const auto one = std::make_shared<ui::Span>("kot");

    top.children().add(one);

    SAME(one->role(), std::string{"talcite.normal"});
    SAME(top.render({{0, 0}, {5, 1}}).at({0, 0}).style.foreground, (Color{false, 1, 1, 1}));

    one->role("moja.rola");

    SAME(top.render({{0, 0}, {5, 1}}).at({0, 0}).style.foreground, (Color{false, 9, 9, 9}));
}

TRIAL("Role: a frame asks for the stem it was given") {
    ui::Root top;

    Style mine{};

    mine.background = Color{false, 7, 7, 7};

    top.skin(ui::Skin{{{"talcite.frame", inked(1, 1, 1)}, {"moja.ramka", mine}}});

    const auto one = std::make_shared<ui::Frame>();

    top.children().add(one);

    one->role("moja.ramka");

    const Snapshot made = top.render({{0, 0}, {6, 3}});

    // The line and the background come from the same role.
    SAME(made.at({0, 0}).style.background, (Color{false, 7, 7, 7}));
    SAME(made.at({2, 1}).style.background, (Color{false, 7, 7, 7}));
}

TRIAL("Role: a button adds a suffix for its state") {
    ui::Root top;

    ui::Styles these;

    these["moj.guzik"] = inked(1, 1, 1);
    these["moj.guzik.focus"] = inked(2, 2, 2);
    these["moj.guzik.disabled"] = inked(3, 3, 3);

    top.skin(ui::Skin{these});

    const auto one = std::make_shared<ui::Button>("Tak");

    top.children().add(one);

    SAME(one->role(), std::string{"talcite.button"});

    one->role("moj.guzik");

    SAME(top.render({{0, 0}, {9, 1}}).at({0, 0}).style.foreground, (Color{false, 1, 1, 1}));

    one->focus();

    SAME(top.render({{0, 0}, {9, 1}}).at({0, 0}).style.foreground, (Color{false, 2, 2, 2}));

    one->enabled(false);

    SAME(top.render({{0, 0}, {9, 1}}).at({0, 0}).style.foreground, (Color{false, 3, 3, 3}));
}

TRIAL("Role: the hotkey letter also follows the stem") {
    ui::Root top;

    ui::Styles these;

    Style hot{};

    hot.reverse = true;

    these["moj.guzik"] = inked(1, 1, 1);
    these["moj.guzik.key"] = hot;

    top.skin(ui::Skin{these});

    const auto one = std::make_shared<ui::Button>("Tak");

    one->keys().add(U'T');
    one->role("moj.guzik");

    top.children().add(one);

    const Snapshot made = top.render({{0, 0}, {9, 1}});

    // `[ Tak ]`: the letter T stands in the third cell.
    SAME(made.at({2, 0}).letter, U'T');
    OUGHT(made.at({2, 0}).style.reverse);
    OUGHT(!made.at({3, 0}).style.reverse);
}

// The hotkey letter changes the foreground and the attributes, never the background. Midnight
// Commander skins give `.key` a foreground and background pair, and MC has a separate role for the
// letter in a highlighted position, which we do not have. Taking the background from `.key` would
// punch a hole into the highlight: an orange `L` on the bar's background and eft on purple. The
// trial puts a background into `.key` that differs from the focus background and checks the letter's
// cell.
TRIAL("Role: the hotkey letter takes the background beneath it and the foreground from .key") {
    ui::Root top;

    ui::Styles these;

    Style plain = tinted(1, 1, 1);
    Style focus = tinted(2, 2, 2);
    Style key = tinted(9, 9, 9);

    plain.background = Color{false, 10, 10, 10};
    focus.background = Color{false, 20, 20, 20};
    key.background = Color{false, 10, 10, 10};
    key.bold = true;

    these["moj.guzik"] = plain;
    these["moj.guzik.focus"] = focus;
    these["moj.guzik.key"] = key;

    top.skin(ui::Skin{these});

    const auto one = std::make_shared<ui::Button>("Tak");

    one->keys().add(U'T');
    one->role("moj.guzik");

    top.children().add(one);

    // Without focus: foreground from `.key`, the button's background, here by chance the same as in
    // `.key`.
    Snapshot made = top.render({{0, 0}, {9, 1}});

    SAME(made.at({2, 0}).letter, U'T');
    SAME(made.at({2, 0}).style.foreground, (Color{false, 9, 9, 9}));
    SAME(made.at({2, 0}).style.background, (Color{false, 10, 10, 10}));
    OUGHT(made.at({2, 0}).style.bold);

    // With focus: the foreground still from `.key`, but the background of focus, not from `.key`.
    one->focus();

    made = top.render({{0, 0}, {9, 1}});

    SAME(made.at({2, 0}).style.foreground, (Color{false, 9, 9, 9}));
    SAME(made.at({2, 0}).style.background, (Color{false, 20, 20, 20}));
    SAME(made.at({3, 0}).style.background, (Color{false, 20, 20, 20}));
    OUGHT(made.at({2, 0}).style.bold);
}

// The message family and the error family have to be in the built-in skin, otherwise a focused
// button in a message would lose its highlight: it would ask for a role nobody has heard of.
TRIAL("Skin: the built-in skin knows the message and error families") {
    const ui::Skin one;

    for (const std::string& stem : {std::string{"talcite.message"}, std::string{"talcite.error"}}) {
        OUGHT(one.style(stem + ".button.focus").reverse);
        OUGHT(one.style(stem + ".button.key").under);
        OUGHT(one.style(stem + ".button.disabled").faint);

        // The rest of the family is empty on purpose: without colors an error box looks no different
        // from a plain window, just as in Midnight Commander's mono skin.
        SAME(one.style(stem), Style{});
        SAME(one.style(stem + ".title"), Style{});
        SAME(one.style(stem + ".text"), Style{});
        SAME(one.style(stem + ".box"), Style{});
    }
}


// --- Variants for terminal levels ----------------------------------------------------------
//
// A skin carries a role's value for each level of color and glyphs; which one applies is told by
// its root from the facts about the terminal. With no variant, the built-in value applies, with no
// approximation.

namespace {

// A role with an `ansi` variant and an `rgb` variant, and nothing for the other levels.
ui::Shades shaded() {
    ui::Shades made;

    made.ansi = tinted(1, 1, 1);
    made.rgb = tinted(3, 3, 3);

    return made;
}

}

TRIAL("Skin: a single value applies at every level") {
    ui::Skin one{{{"talcite.list.focus", tinted(10, 20, 30)}}};

    for (const Colors level : {Colors::mono, Colors::ansi, Colors::palette, Colors::rgb}) {
        one.colors(level);

        SAME(one.style("talcite.list.focus"), tinted(10, 20, 30));
    }
}

TRIAL("Skin: the variant is the highest one not above the level") {
    ui::Skin one{{{"talcite.list.focus", shaded()}}};

    one.colors(Colors::ansi);
    SAME(one.style("talcite.list.focus"), tinted(1, 1, 1));

    // `palette` has no variant of its own, so `ansi` applies, not `rgb`: it never reaches higher.
    one.colors(Colors::palette);
    SAME(one.style("talcite.list.focus"), tinted(1, 1, 1));

    one.colors(Colors::rgb);
    SAME(one.style("talcite.list.focus"), tinted(3, 3, 3));
}

TRIAL("Skin: with no variant for the level, the role goes back to the built-in one, not an approximation") {
    ui::Skin one{{{"talcite.list.focus", shaded()}}};

    // `mono` has nothing below it, and the skin starts at `ansi`, so the built-in value applies:
    // reverse video.
    one.colors(Colors::mono);

    SAME(one.style("talcite.list.focus"), ui::Skin::plain().style("talcite.list.focus"));
    OUGHT(one.style("talcite.list.focus").reverse);
    OUGHT(one.style("talcite.list.focus").foreground.transparent);
}

TRIAL("Skin: a role outside the built-in ones with no variant gives an empty style") {
    ui::Skin one{{{"app.thing", shaded()}}};

    one.colors(Colors::mono);

    SAME(one.style("app.thing"), Style{});
}

TRIAL("Skin: a glyph has an ASCII and a Unicode version, a terminal without Unicode takes only ASCII") {
    ui::Signs sign;

    sign.ascii = U'-';
    sign.unicode = U'─';

    ui::Skin one{{}, {{"talcite.line.thin.across", sign}}};

    SAME(one.glyph("talcite.line.thin.across"), U'-');
    SAME(one.lines(ui::Border::thin).across, U'-');

    one.unicode(true);

    SAME(one.glyph("talcite.line.thin.across"), U'─');
    SAME(one.lines(ui::Border::thin).across, U'─');
}

TRIAL("Skin: a glyph with only a Unicode version falls back to the built-in one without Unicode") {
    ui::Signs sign;

    sign.unicode = U'=';

    ui::Skin one{{}, {{"talcite.line.thin.across", sign}}};

    SAME(one.glyph("talcite.line.thin.across"), U'─');

    one.unicode(true);

    SAME(one.glyph("talcite.line.thin.across"), U'=');
}

TRIAL("Root: facts about the terminal carry over to every later skin") {
    ui::Root top;

    top.colors(Colors::rgb);
    top.unicode(true);

    top.skin(ui::Skin{{{"talcite.list.focus", shaded()}}});

    SAME(static_cast<int>(top.skin().colors()), static_cast<int>(Colors::rgb));
    OUGHT(top.skin().unicode());
    SAME(top.skin().style("talcite.list.focus"), tinted(3, 3, 3));

    top.colors(Colors::ansi);

    SAME(top.skin().style("talcite.list.focus"), tinted(1, 1, 1));
}

TRIAL("Root: without facts the skin stands on mono and ASCII") {
    ui::Root top;

    SAME(static_cast<int>(top.skin().colors()), static_cast<int>(Colors::mono));
    OUGHT(!top.skin().unicode());
}

// The 256-color palette: sixteen basic colors in xterm's values, the 6×6×6 cube and the gray ramp.
// Outside the palette, nothing. The terminal computes from it when writing, and the application
// reads colors from it.
TRIAL("Palette: Color::indexed gives the basic colors, the cube and the grays, and nothing outside") {
    const auto rgb = [](int which) {
        const std::optional<Color> one = Color::indexed(which);

        return one ? std::to_string(one->red) + "," + std::to_string(one->green) + "," + std::to_string(one->blue)
                   : std::string{"-"};
    };

    SAME(rgb(0), std::string{"0,0,0"});
    SAME(rgb(4), std::string{"0,0,128"});
    SAME(rgb(7), std::string{"192,192,192"});
    SAME(rgb(8), std::string{"128,128,128"});
    SAME(rgb(15), std::string{"255,255,255"});
    SAME(rgb(16), std::string{"0,0,0"});
    SAME(rgb(17), std::string{"0,0,95"});
    SAME(rgb(52), std::string{"95,0,0"});
    SAME(rgb(231), std::string{"255,255,255"});
    SAME(rgb(232), std::string{"8,8,8"});
    SAME(rgb(255), std::string{"238,238,238"});
    SAME(rgb(-1), std::string{"-"});
    SAME(rgb(256), std::string{"-"});
    OUGHT(!Color::indexed(3)->transparent);
}
