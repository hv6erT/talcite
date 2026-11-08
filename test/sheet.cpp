#include "tal/testing.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "talcite/canvas.hpp"
#include "talcite/encoding.hpp"
#include "talcite/snapshot.hpp"

#include "sheet.hpp"

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
detail::Sheet painted(ui::Size how, char32_t what) {
    detail::Sheet made{how};

    for (int y = 0; y < how.high; ++y)
        for (int x = 0; x < how.wide; ++x) made.put({x, y}, what, Style{});

    return made;
}

}

// `Canvas::put` has two overloads: a character and a string. Every spelling one writes by reflex (a
// character literal, a string literal, `std::string`) has to land where it means, without a cast.
TRIAL("Canvas: put tells a character from a string") {
    detail::Sheet one{{6, 1}};
    Canvas onto{one};

    onto.put({0, 0}, U'ż', marked());
    onto.put({1, 0}, 'a', Style{});
    onto.put({2, 0}, "bc", Style{});
    onto.put({4, 0}, std::string{"d"}, Style{});
    onto.put({5, 0}, std::string_view{"e"}, Style{});

    SAME(one.at({0, 0}).letter, U'ż');
    OUGHT(one.at({0, 0}).style == marked());
    SAME(one.at({1, 0}).letter, U'a');
    SAME(one.at({2, 0}).letter, U'b');
    SAME(one.at({3, 0}).letter, U'c');
    SAME(one.at({4, 0}).letter, U'd');
    SAME(one.at({5, 0}).letter, U'e');
}

TRIAL("Sheet: a fresh page has its size and nothing but spaces") {
    const detail::Sheet one{{4, 2}};

    SAME(one.size().wide, 4);
    SAME(one.size().high, 2);
    SAME(one.at({0, 0}).letter, U' ');
    SAME(one.at({3, 1}).letter, U' ');
}

TRIAL("Sheet: a negative dimension goes down to zero, not to an exception") {
    const detail::Sheet one{{-5, 3}};

    SAME(one.size().wide, 0);
    SAME(one.size().high, 3);
    SAME(one.at({0, 0}).letter, U' ');
}

TRIAL("Sheet: a letter stands where it was told") {
    detail::Sheet one{{4, 2}};

    one.put({2, 1}, U'A', marked());

    SAME(one.at({2, 1}).letter, U'A');
    OUGHT(one.at({2, 1}).style.bold);
    SAME(one.at({1, 1}).letter, U' ');
}

TRIAL("Sheet: writing outside the page does nothing") {
    detail::Sheet one{{2, 2}};

    one.put({5, 0}, U'A', Style{});
    one.put({-1, 0}, U'B', Style{});
    one.put({0, 9}, U'C', Style{});

    SAME(one.at({0, 0}).letter, U' ');
    SAME(one.at({1, 1}).letter, U' ');
}

TRIAL("Canvas: a string goes to the right") {
    detail::Sheet one{{6, 1}};
    Canvas onto{one};

    onto.put({1, 0}, "abc", Style{});

    SAME(one.at({0, 0}).letter, U' ');
    SAME(one.at({1, 0}).letter, U'a');
    SAME(one.at({2, 0}).letter, U'b');
    SAME(one.at({3, 0}).letter, U'c');
}

TRIAL("Canvas: a string with diacritics counts characters, not bytes") {
    detail::Sheet one{{4, 1}};
    Canvas onto{one};

    onto.put({0, 0}, "żółw", Style{});

    SAME(one.at({0, 0}).letter, U'ż');
    SAME(one.at({1, 0}).letter, U'ó');
    SAME(one.at({2, 0}).letter, U'ł');
    SAME(one.at({3, 0}).letter, U'w');
}

TRIAL("Canvas: a string is cut off at the edge") {
    detail::Sheet one{{3, 1}};
    Canvas onto{one};

    onto.put({1, 0}, "abcdef", Style{});

    SAME(one.at({1, 0}).letter, U'a');
    SAME(one.at({2, 0}).letter, U'b');
    SAME(one.size().wide, 3);
}

TRIAL("Canvas: broken UTF-8 gives a replacement character instead of vanishing") {
    detail::Sheet one{{4, 1}};
    Canvas onto{one};

    std::string said = "a";
    said += static_cast<char>(0xFF);
    said += "b";

    onto.put({0, 0}, said, Style{});

    SAME(one.at({0, 0}).letter, U'a');
    SAME(one.at({1, 0}).letter, U'�');
    SAME(one.at({2, 0}).letter, U'b');
}

TRIAL("Sheet: a double-width character takes two cells") {
    detail::Sheet one{{4, 1}};

    one.put({1, 0}, U'漢', Style{});

    SAME(one.at({1, 0}).letter, U'漢');
    SAME(one.at({2, 0}).letter, static_cast<char32_t>(0));
}

TRIAL("Sheet: a double-width character in the last column stays a space") {
    detail::Sheet one{{3, 1}};

    one.put({2, 0}, U'漢', Style{});

    SAME(one.at({2, 0}).letter, U' ');
}

TRIAL("Sheet: overwriting half of a double-width character cleans up the other half") {
    detail::Sheet one{{4, 1}};

    one.put({0, 0}, U'漢', Style{});
    one.put({1, 0}, U'x', Style{});

    SAME(one.at({0, 0}).letter, U' ');
    SAME(one.at({1, 0}).letter, U'x');
}

TRIAL("Sheet: overwriting the first half cleans up the continuation") {
    detail::Sheet one{{4, 1}};

    one.put({1, 0}, U'漢', Style{});
    one.put({1, 0}, U'x', Style{});

    SAME(one.at({1, 0}).letter, U'x');
    SAME(one.at({2, 0}).letter, U' ');
}

TRIAL("Sheet: fill paints a rectangle and does not go outside it") {
    detail::Sheet one{{4, 3}};

    one.fill({{1, 1}, {2, 1}}, marked());

    OUGHT(one.at({1, 1}).style.bold);
    OUGHT(one.at({2, 1}).style.bold);
    OUGHT(!one.at({0, 1}).style.bold);
    OUGHT(!one.at({1, 0}).style.bold);
}

TRIAL("Sheet: fill cuts a double-width character at both of its edges") {
    detail::Sheet one{{6, 1}};
    one.put({0, 0}, U'漢', Style{});
    one.put({2, 0}, U'漢', Style{});
    one.put({4, 0}, U'漢', Style{});

    one.fill({{1, 0}, {4, 1}}, marked());

    SAME(one.at({0, 0}).letter, U' ');
    OUGHT(!one.at({0, 0}).style.bold);

    for (int x = 1; x < 5; ++x) {
        SAME(one.at({x, 0}).letter, U' ');
        OUGHT(one.at({x, 0}).style.bold);
    }

    SAME(one.at({5, 0}).letter, U' ');
    OUGHT(!one.at({5, 0}).style.bold);
}

TRIAL("Sheet: fill and clear reaching past the page stop at its edge") {
    detail::Sheet one{{3, 2}};

    one.fill({{-2, -1}, {9, 9}}, marked());

    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 3; ++x) OUGHT(one.at({x, y}).style.bold);

    one.clear({{2, -5}, {7, 6}});

    SAME(static_cast<int>(one.at({2, 0}).shown), static_cast<int>(Shown::none));
    OUGHT(one.at({1, 0}).style.bold);
    SAME(static_cast<int>(one.at({2, 1}).shown), static_cast<int>(Shown::own));
}

TRIAL("Sheet: clearing half of a double-width character leaves no half behind") {
    detail::Sheet one{{4, 1}};
    one.put({0, 0}, U'漢', Style{});
    one.put({2, 0}, U'漢', Style{});

    one.clear({{1, 0}, {2, 1}});

    SAME(one.at({0, 0}).letter, U' ');
    SAME(one.at({3, 0}).letter, U' ');

    one.put({1, 0}, U'y', Style{});

    SAME(one.at({0, 0}).letter, U' ');
    SAME(one.at({1, 0}).letter, U'y');
}

TRIAL("Sheet: asking for a cell outside the page gives an empty one") {
    const detail::Sheet one{{2, 2}};

    SAME(one.at({9, 9}).letter, U' ');
    SAME(one.at({-1, 0}).letter, U' ');
}

// A file name, a title from the network, a description from an NFO: foreign text goes to the
// screen, not to the terminal as a command. C0 and DEL show as their pictures, C1 and bidirectional
// control characters as a replacement character, and the width stays one cell, so the columns do
// not drift apart.
TRIAL("Canvas: control characters become visible instead of being executed") {
    detail::Sheet one{{9, 1}};
    Canvas onto{one};

    onto.put({0, 0}, std::string{"a\x1b]2;\x07"} + "\xc2\x9b" + "\xe2\x80\xae" + "\x7f" + "z", Style{});

    // As numbers, not literals: the compiler rightly warns about direction characters in source.
    constexpr char32_t substitute = 0xFFFD;

    SAME(one.at({0, 0}).letter, U'a');
    SAME(one.at({1, 0}).letter, char32_t{0x241B});
    SAME(one.at({2, 0}).letter, U']');
    SAME(one.at({4, 0}).letter, U';');
    SAME(one.at({5, 0}).letter, char32_t{0x2407});
    SAME(one.at({6, 0}).letter, substitute);
    SAME(one.at({7, 0}).letter, substitute);
    SAME(one.at({8, 0}).letter, char32_t{0x2421});

    for (const char32_t each : {0x200Eu, 0x200Fu, 0x061Cu, 0x2028u, 0x2029u, 0x202Au, 0x202Eu, 0x2066u, 0x2069u, 0x80u, 0x9Fu})
        SAME(shown(each), substitute);

    for (const char32_t each : {0x20u, 0x7Eu, 0xA0u, 0x17Cu, 0x2400u, 0x206Au, 0x2027u, 0x6F22u}) SAME(shown(each), each);

    SAME(shown(0), char32_t{0x2400});
    SAME(shown(0x1F), char32_t{0x241F});
}

// --- Hyperlinks ----------------------------------------------------------------------------------

// A link marks the cells that show something and keeps its address in the list; a cell the node did
// not draw stays without it, so a link never reaches over what lies below.
TRIAL("Sheet: a hyperlink marks the cells that show something, and the list keeps its address") {
    detail::Sheet one{{6, 1}};

    one.clear({{0, 0}, {6, 1}});
    Canvas{one}.put({0, 0}, "abc", Style{});

    const Hyperlink made = hyperlink("https://example.org/");

    one.link({{0, 0}, {5, 1}}, made);

    SAME(one.at({0, 0}).link, made.mark);
    SAME(one.at({2, 0}).link, made.mark);
    SAME(one.at({3, 0}).link, std::uint32_t{0});
    SAME(one.links().size(), std::size_t{1});
    SAME(one.links().front().address, std::string{"https://example.org/"});

    // The same link once more is one entry, not two.
    one.link({{0, 0}, {1, 1}}, made);

    SAME(one.links().size(), std::size_t{1});
}

// One address keeps one mark, frame after frame and whoever draws it, so that frames compare equal;
// two addresses never share one, so that a new address rewrites the cells. None is zero.
TRIAL("Sheet: one address keeps its hyperlink mark, two addresses get two") {
    const Hyperlink one = hyperlink("https://a.example/");
    const Hyperlink again = hyperlink("https://a.example/");
    const Hyperlink two = hyperlink("https://b.example/");

    OUGHT(one.mark != 0);
    SAME(again.mark, one.mark);
    OUGHT(two.mark != one.mark);
    OUGHT(two.mark != 0);
}

