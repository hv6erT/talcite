#include "proving.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "talcite/encoding.hpp"
#include "talcite/view.hpp"

using namespace talcite;

namespace {

// A style other than the default, so that it can be checked that it travels with the character.
Style marked() {
    Style made;
    made.bold = true;
    made.foreground = Color{false, 200, 100, 50};

    return made;
}

// A page filled with one letter, the shortest way to get a child to paste.
View painted(ui::Size how, char32_t what) {
    View made{how};

    for (int y = 0; y < how.high; ++y)
        for (int x = 0; x < how.wide; ++x) made.put({x, y}, what, Style{});

    return made;
}

}

// `put` has two overloads: a character and a string. Every spelling one writes by reflex (a
// character literal, a string literal, `std::string`) has to land where it means, without a cast.
TRIAL("View: put tells a character from a string") {
    View one{{6, 1}};

    one.put({0, 0}, U'ż', marked());
    one.put({1, 0}, 'a', Style{});
    one.put({2, 0}, "bc", Style{});
    one.put({4, 0}, std::string{"d"}, Style{});
    one.put({5, 0}, std::string_view{"e"}, Style{});

    same(one.at({0, 0}).letter, U'ż');
    ought(one.at({0, 0}).style == marked());
    same(one.at({1, 0}).letter, U'a');
    same(one.at({2, 0}).letter, U'b');
    same(one.at({3, 0}).letter, U'c');
    same(one.at({4, 0}).letter, U'd');
    same(one.at({5, 0}).letter, U'e');
}

TRIAL("View: a fresh page has its size and nothing but spaces") {
    const View one{{4, 2}};

    same(one.size().wide, 4);
    same(one.size().high, 2);
    same(one.at({0, 0}).letter, U' ');
    same(one.at({3, 1}).letter, U' ');
}

TRIAL("View: a negative dimension goes down to zero, not to an exception") {
    const View one{{-5, 3}};

    same(one.size().wide, 0);
    same(one.size().high, 3);
    same(one.at({0, 0}).letter, U' ');
}

TRIAL("View: a letter stands where it was told") {
    View one{{4, 2}};

    one.put({2, 1}, U'A', marked());

    same(one.at({2, 1}).letter, U'A');
    ought(one.at({2, 1}).style.bold);
    same(one.at({1, 1}).letter, U' ');
}

TRIAL("View: writing outside the page does nothing") {
    View one{{2, 2}};

    one.put({5, 0}, U'A', Style{});
    one.put({-1, 0}, U'B', Style{});
    one.put({0, 9}, U'C', Style{});

    same(one.at({0, 0}).letter, U' ');
    same(one.at({1, 1}).letter, U' ');
}

TRIAL("View: a string goes to the right") {
    View one{{6, 1}};

    one.put({1, 0}, "abc", Style{});

    same(one.at({0, 0}).letter, U' ');
    same(one.at({1, 0}).letter, U'a');
    same(one.at({2, 0}).letter, U'b');
    same(one.at({3, 0}).letter, U'c');
}

TRIAL("View: a string with diacritics counts characters, not bytes") {
    View one{{4, 1}};

    one.put({0, 0}, "żółw", Style{});

    same(one.at({0, 0}).letter, U'ż');
    same(one.at({1, 0}).letter, U'ó');
    same(one.at({2, 0}).letter, U'ł');
    same(one.at({3, 0}).letter, U'w');
}

TRIAL("View: a string is cut off at the edge") {
    View one{{3, 1}};

    one.put({1, 0}, "abcdef", Style{});

    same(one.at({1, 0}).letter, U'a');
    same(one.at({2, 0}).letter, U'b');
    same(one.size().wide, 3);
}

TRIAL("View: broken UTF-8 gives a replacement character instead of vanishing") {
    View one{{4, 1}};

    std::string said = "a";
    said += static_cast<char>(0xFF);
    said += "b";

    one.put({0, 0}, said, Style{});

    same(one.at({0, 0}).letter, U'a');
    same(one.at({1, 0}).letter, U'�');
    same(one.at({2, 0}).letter, U'b');
}

TRIAL("View: a double-width character takes two cells") {
    View one{{4, 1}};

    one.put({1, 0}, U'漢', Style{});

    same(one.at({1, 0}).letter, U'漢');
    same(one.at({2, 0}).letter, static_cast<char32_t>(0));
}

TRIAL("View: a double-width character in the last column stays a space") {
    View one{{3, 1}};

    one.put({2, 0}, U'漢', Style{});

    same(one.at({2, 0}).letter, U' ');
}

TRIAL("View: overwriting half of a double-width character cleans up the other half") {
    View one{{4, 1}};

    one.put({0, 0}, U'漢', Style{});
    one.put({1, 0}, U'x', Style{});

    same(one.at({0, 0}).letter, U' ');
    same(one.at({1, 0}).letter, U'x');
}

TRIAL("View: overwriting the first half cleans up the continuation") {
    View one{{4, 1}};

    one.put({1, 0}, U'漢', Style{});
    one.put({1, 0}, U'x', Style{});

    same(one.at({1, 0}).letter, U'x');
    same(one.at({2, 0}).letter, U' ');
}

TRIAL("View: fill paints a rectangle and does not go outside it") {
    View one{{4, 3}};

    one.fill({{1, 1}, {2, 1}}, marked());

    ought(one.at({1, 1}).style.bold);
    ought(one.at({2, 1}).style.bold);
    ought(!one.at({0, 1}).style.bold);
    ought(!one.at({1, 0}).style.bold);
}

TRIAL("View: fill cuts a double-width character at both of its edges") {
    View one{{6, 1}};
    one.put({0, 0}, U'漢', Style{});
    one.put({2, 0}, U'漢', Style{});
    one.put({4, 0}, U'漢', Style{});

    one.fill({{1, 0}, {4, 1}}, marked());

    same(one.at({0, 0}).letter, U' ');
    ought(!one.at({0, 0}).style.bold);

    for (int x = 1; x < 5; ++x) {
        same(one.at({x, 0}).letter, U' ');
        ought(one.at({x, 0}).style.bold);
    }

    same(one.at({5, 0}).letter, U' ');
    ought(!one.at({5, 0}).style.bold);
}

TRIAL("View: fill and clear reaching past the page stop at its edge") {
    View one{{3, 2}};

    one.fill({{-2, -1}, {9, 9}}, marked());

    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 3; ++x) ought(one.at({x, y}).style.bold);

    one.clear({{2, -5}, {7, 6}});

    same(static_cast<int>(one.at({2, 0}).shown), static_cast<int>(Shown::none));
    ought(one.at({1, 0}).style.bold);
    same(static_cast<int>(one.at({2, 1}).shown), static_cast<int>(Shown::own));
}

TRIAL("View: a shade changes the style but keeps the letter from below") {
    View one = painted({4, 1}, U'x');

    View two{{4, 1}};
    two.clear({{0, 0}, {4, 1}});

    Style dark;
    dark.faint = true;

    two.shade({{1, 0}, {2, 1}}, dark);

    one.draw(two, {0, 0});

    same(one.at({0, 0}).letter, U'x');
    ought(!one.at({0, 0}).style.faint);
    same(one.at({1, 0}).letter, U'x');
    ought(one.at({1, 0}).style.faint);
    ought(one.at({2, 0}).style.faint);
    ought(!one.at({3, 0}).style.faint);
}

TRIAL("View: a shade over emptiness draws like a plain cell") {
    View one{{2, 1}};

    View two{{2, 1}};

    Style dark;
    dark.faint = true;

    two.shade({{0, 0}, {2, 1}}, dark);

    one.draw(two, {0, 0});

    same(one.at({0, 0}).letter, U' ');
    ought(one.at({0, 0}).style.faint);
}

// A shade over emptiness has to stay a shade, not vanish. A cell nobody has drawn on yet says "I am
// not here", and pasting skips such cells, so the dimming would be lost one level up and the shadow
// under a dialog would break off halfway. Containers do not paint over their rectangles, so such
// emptiness under a shade is common.
TRIAL("View: a shade through emptiness reaches the level above") {
    View under = painted({3, 1}, U'x');

    // The middle level: nothing at all.
    View middle{{3, 1}};
    middle.clear({{0, 0}, {3, 1}});

    // The upper level: only the shade.
    View above{{3, 1}};
    above.clear({{0, 0}, {3, 1}});

    Style dark;
    dark.faint = true;

    above.shade({{1, 0}, {1, 1}}, dark);

    middle.draw(above, {0, 0});
    under.draw(middle, {0, 0});

    same(under.at({1, 0}).letter, U'x');
    ought(under.at({1, 0}).style.faint);
    ought(!under.at({0, 0}).style.faint);
    ought(!under.at({2, 0}).style.faint);
}

// Foreground only (the cell at the edge of an image): the character and its color are its own, the
// background comes from below. Text does not do that, since its role gives its own background, so
// this is a separate kind of cell, not a new pasting rule.
TRIAL("View: the foreground takes the background from below and keeps its own character and color") {
    View one{{2, 1}};

    Style blue;

    blue.background = Color{false, 0, 0, 160};

    one.fill({{0, 0}, {2, 1}}, blue);

    View two{{2, 1}};
    two.clear({{0, 0}, {2, 1}});

    Style red;

    red.foreground = Color{false, 200, 0, 0};

    two.foreground({0, 0}, U'▌', red);

    // A zero character means no character; without one the foreground lays down nothing.
    two.foreground({1, 0}, 0, red);

    same(static_cast<int>(two.at({1, 0}).shown), static_cast<int>(Shown::none));

    one.draw(two, {0, 0});

    same(one.at({0, 0}).letter, U'▌');
    same(one.at({0, 0}).style.foreground, (Color{false, 200, 0, 0}));
    same(one.at({0, 0}).style.background, (Color{false, 0, 0, 160}));
    same(static_cast<int>(one.at({0, 0}).shown), static_cast<int>(Shown::own));
    same(one.at({1, 0}).style.background, (Color{false, 0, 0, 160}));

    // A plain cell with the same style covers the background from below with a transparent one,
    // just like text.
    View three{{2, 1}};
    three.clear({{0, 0}, {2, 1}});
    three.put({0, 0}, U'▌', red);

    one.draw(three, {0, 0});

    ought(one.at({0, 0}).style.background.transparent);
}

TRIAL("View: the foreground through emptiness reaches the level above, and with a background given it is a plain cell") {
    View under{{2, 1}};

    Style blue;

    blue.background = Color{false, 0, 0, 160};

    under.fill({{0, 0}, {2, 1}}, blue);

    View middle{{2, 1}};
    middle.clear({{0, 0}, {2, 1}});

    View above{{2, 1}};
    above.clear({{0, 0}, {2, 1}});

    Style red;

    red.foreground = Color{false, 200, 0, 0};

    Style papered = red;

    papered.background = Color{false, 0, 120, 0};

    above.foreground({0, 0}, U'▐', red);
    above.foreground({1, 0}, U'▐', papered);

    same(static_cast<int>(above.at({0, 0}).shown), static_cast<int>(Shown::foreground));
    same(static_cast<int>(above.at({1, 0}).shown), static_cast<int>(Shown::own));

    middle.draw(above, {0, 0});

    same(static_cast<int>(middle.at({0, 0}).shown), static_cast<int>(Shown::foreground));

    under.draw(middle, {0, 0});

    same(under.at({0, 0}).letter, U'▐');
    same(under.at({0, 0}).style.background, (Color{false, 0, 0, 160}));
    same(under.at({1, 0}).style.background, (Color{false, 0, 120, 0}));

    // Outside the page the foreground does nothing.
    above.foreground({5, 0}, U'x', red);

    same(above.size().wide, 2);
}

TRIAL("View: an undrawn cell changes nothing") {
    View one = painted({3, 1}, U'x');

    View two{{3, 1}};
    two.put({0, 0}, U'a', marked());
    two.clear({{1, 0}, {2, 1}});

    one.draw(two, {0, 0});

    same(one.at({0, 0}).letter, U'a');
    same(one.at({1, 0}).letter, U'x');
    same(one.at({2, 0}).letter, U'x');
    ought(!one.at({1, 0}).style.bold);
}

TRIAL("View: painting after emptiness covers again") {
    View one = painted({2, 1}, U'x');

    View two{{2, 1}};
    two.clear({{0, 0}, {2, 1}});
    two.put({1, 0}, U'b', Style{});

    one.draw(two, {0, 0});

    same(one.at({0, 0}).letter, U'x');
    same(one.at({1, 0}).letter, U'b');
}

TRIAL("View: pasting puts another page at an offset") {
    View one{{6, 4}};

    one.draw(painted({2, 2}, U'x'), {3, 1});

    same(one.at({3, 1}).letter, U'x');
    same(one.at({4, 2}).letter, U'x');
    same(one.at({2, 1}).letter, U' ');
    same(one.at({3, 3}).letter, U' ');
}

TRIAL("View: pasting clips what sticks out") {
    View one{{3, 2}};

    one.draw(painted({4, 4}, U'x'), {2, 1});

    same(one.at({2, 1}).letter, U'x');
    same(one.size().wide, 3);
    same(one.size().high, 2);
}

TRIAL("View: pasting with a negative offset takes only the visible part") {
    View one{{3, 2}};

    View two{{3, 2}};
    two.put({0, 0}, U'a', Style{});
    two.put({2, 1}, U'b', Style{});

    one.draw(two, {-1, -1});

    same(one.at({1, 0}).letter, U'b');
    same(one.at({0, 0}).letter, U' ');
}

TRIAL("View: pasting completely beside it leaves the page alone") {
    View one{{3, 2}};

    one.draw(painted({2, 2}, U'x'), {9, 9});

    same(one.at({0, 0}).letter, U' ');
    same(one.at({2, 1}).letter, U' ');
}

TRIAL("View: the edge of a paste leaves no half of a double-width character") {
    View one{{3, 1}};

    View two{{4, 1}};
    two.put({2, 0}, U'漢', Style{});

    one.draw(two, {0, 0});

    same(one.at({2, 0}).letter, U' ');
}

// A window is a view with a transparent rim and a shadow, so the edge of what it covers lies inside
// its rectangle. A double-width character below is cut there and nowhere else: one under the rim
// stays whole.
TRIAL("View: a window cuts double-width characters below only at its own edge") {
    View one{{8, 1}};
    one.put({0, 0}, U'映', Style{});
    one.put({2, 0}, U'画', Style{});
    one.put({4, 0}, U'の', Style{});
    one.put({6, 0}, U'名', Style{});

    View two{{8, 1}};
    two.clear({{0, 0}, {8, 1}});
    two.put({3, 0}, U'|', Style{});
    two.put({4, 0}, U'x', Style{});
    two.put({5, 0}, U'|', Style{});

    one.draw(two, {0, 0});

    same(one.at({0, 0}).letter, U'映');
    same(one.at({1, 0}).letter, char32_t{0});
    same(one.at({2, 0}).letter, U' ');
    same(one.at({3, 0}).letter, U'|');
    same(one.at({4, 0}).letter, U'x');
    same(one.at({5, 0}).letter, U'|');
    same(one.at({6, 0}).letter, U'名');
    same(one.at({7, 0}).letter, char32_t{0});
}

TRIAL("View: a shadow keeps a double-width character whole and a window cuts it") {
    View one{{6, 1}};
    one.put({0, 0}, U'映', Style{});
    one.put({2, 0}, U'画', Style{});
    one.put({4, 0}, U'名', Style{});

    Style dark;
    dark.faint = true;

    View two{{6, 1}};
    two.clear({{0, 0}, {6, 1}});
    two.put({1, 0}, U'a', Style{});
    two.shade({{2, 0}, {2, 1}}, dark);
    two.put({4, 0}, U'b', Style{});

    one.draw(two, {0, 0});

    same(one.at({0, 0}).letter, U' ');
    same(one.at({1, 0}).letter, U'a');
    same(one.at({2, 0}).letter, U'画');
    same(one.at({3, 0}).letter, char32_t{0});
    ought(one.at({2, 0}).style.faint);
    ought(one.at({3, 0}).style.faint);
    same(one.at({4, 0}).letter, U'b');
    same(one.at({5, 0}).letter, U' ');
}

TRIAL("View: a double-width character drawn without its other half becomes a space") {
    View one = painted({4, 1}, U'x');

    Style dark;
    dark.faint = true;

    View two{{4, 1}};
    two.put({0, 0}, U'漢', Style{});
    two.put({2, 0}, U'漢', Style{});
    two.shade({{1, 0}, {1, 1}}, dark);
    two.shade({{2, 0}, {1, 1}}, dark);

    one.draw(two, {0, 0});

    same(one.at({0, 0}).letter, U' ');
    same(one.at({1, 0}).letter, U'x');
    ought(one.at({1, 0}).style.faint);
    same(one.at({2, 0}).letter, U'x');
    same(one.at({3, 0}).letter, U' ');

    View three = painted({3, 1}, U'x');

    View four{{3, 1}};
    four.put({0, 0}, U'漢', Style{});
    four.put({2, 0}, U'z', Style{});

    three.draw(four, {-1, 0});

    same(three.at({0, 0}).letter, U' ');
    same(three.at({1, 0}).letter, U'z');
    same(three.at({2, 0}).letter, U'x');
}

TRIAL("View: clearing half of a double-width character leaves no half behind") {
    View one{{4, 1}};
    one.put({0, 0}, U'漢', Style{});
    one.put({2, 0}, U'漢', Style{});

    one.clear({{1, 0}, {2, 1}});

    same(one.at({0, 0}).letter, U' ');
    same(one.at({3, 0}).letter, U' ');

    one.put({1, 0}, U'y', Style{});

    same(one.at({0, 0}).letter, U' ');
    same(one.at({1, 0}).letter, U'y');
}

TRIAL("View: asking for a cell outside the page gives an empty one") {
    const View one{{2, 2}};

    same(one.at({9, 9}).letter, U' ');
    same(one.at({-1, 0}).letter, U' ');
}

// A file name, a title from the network, a description from an NFO: foreign text goes to the
// screen, not to the terminal as a command. C0 and DEL show as their pictures, C1 and bidirectional
// control characters as a replacement character, and the width stays one cell, so the columns do
// not drift apart.
TRIAL("View: control characters become visible instead of being executed") {
    View one{{9, 1}};

    one.put({0, 0}, std::string{"a\x1b]2;\x07"} + "\xc2\x9b" + "\xe2\x80\xae" + "\x7f" + "z", Style{});

    // As numbers, not literals: the compiler rightly warns about direction characters in source.
    constexpr char32_t substitute = 0xFFFD;

    same(one.at({0, 0}).letter, U'a');
    same(one.at({1, 0}).letter, char32_t{0x241B});
    same(one.at({2, 0}).letter, U']');
    same(one.at({4, 0}).letter, U';');
    same(one.at({5, 0}).letter, char32_t{0x2407});
    same(one.at({6, 0}).letter, substitute);
    same(one.at({7, 0}).letter, substitute);
    same(one.at({8, 0}).letter, char32_t{0x2421});

    for (const char32_t each : {0x200Eu, 0x200Fu, 0x061Cu, 0x2028u, 0x2029u, 0x202Au, 0x202Eu, 0x2066u, 0x2069u, 0x80u, 0x9Fu})
        same(shown(each), substitute);

    for (const char32_t each : {0x20u, 0x7Eu, 0xA0u, 0x17Cu, 0x2400u, 0x206Au, 0x2027u, 0x6F22u}) same(shown(each), each);

    same(shown(0), char32_t{0x2400});
    same(shown(0x1F), char32_t{0x241F});
}

// --- Hyperlinks ----------------------------------------------------------------------------------

// A link marks the cells that show something and keeps its address in the list; a cell the node did
// not draw stays without it, so a link never reaches over what lies below.
TRIAL("View: a hyperlink marks the cells that show something, and the list keeps its address") {
    View one{{6, 1}};

    one.clear({{0, 0}, {6, 1}});
    one.put({0, 0}, "abc", Style{});

    const Hyperlink made = hyperlink("https://example.org/");

    one.link({{0, 0}, {5, 1}}, made);

    same(one.at({0, 0}).link, made.mark);
    same(one.at({2, 0}).link, made.mark);
    same(one.at({3, 0}).link, std::uint32_t{0});
    same(one.links().size(), std::size_t{1});
    same(one.links().front().address, std::string{"https://example.org/"});

    // The same link once more is one entry, not two.
    one.link({{0, 0}, {1, 1}}, made);

    same(one.links().size(), std::size_t{1});
}

// One address keeps one mark, frame after frame and whoever draws it, so that frames compare equal;
// two addresses never share one, so that a new address rewrites the cells. None is zero.
TRIAL("View: one address keeps its hyperlink mark, two addresses get two") {
    const Hyperlink one = hyperlink("https://a.example/");
    const Hyperlink again = hyperlink("https://a.example/");
    const Hyperlink two = hyperlink("https://b.example/");

    ought(one.mark != 0);
    same(again.mark, one.mark);
    ought(two.mark != one.mark);
    ought(two.mark != 0);
}

// Composing carries the mark with the cell and the address with the list; whatever is put over a
// cell takes the mark away, and a shadow keeps it, since the text below it stays.
TRIAL("View: a hyperlink goes up with the cells, a cover takes it away and a shadow keeps it") {
    View child{{3, 1}};

    child.put({0, 0}, "abc", Style{});

    const Hyperlink made = hyperlink("https://example.org/");

    child.link({{0, 0}, {3, 1}}, made);

    View page{{8, 1}};

    page.draw(child, {2, 0});

    same(page.at({2, 0}).link, made.mark);
    same(page.at({4, 0}).link, made.mark);
    same(page.links().size(), std::size_t{1});

    page.put({2, 0}, U'x', Style{});

    same(page.at({2, 0}).link, std::uint32_t{0});

    View shadow{{1, 1}};

    shadow.shade({{0, 0}, {1, 1}}, Style{});

    page.draw(shadow, {3, 0});

    same(page.at({3, 0}).link, made.mark);

    page.clear({{4, 0}, {1, 1}});

    same(page.at({4, 0}).link, std::uint32_t{0});
}
