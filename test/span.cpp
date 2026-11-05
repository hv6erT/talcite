#include "proving.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include "talcite/encoding.hpp"
#include "talcite/column.hpp"
#include "talcite/root.hpp"
#include "talcite/root.hpp"
#include "talcite/skin.hpp"
#include "talcite/span.hpp"

using namespace talcite;

namespace {

// Reads a row of the drawn page, so that the cases speak about what is visible and not about what
// the layout computed.
std::string read(const Snapshot& one, int y) {
    std::string made;

    for (int x = 0; x < one.size().wide; ++x) {
        const char32_t letter = one.at({x, y}).letter;

        if (letter != 0) made += talcite::spelled(letter);
    }

    while (!made.empty() && made.back() == ' ') made.pop_back();

    return made;
}

}

TRIAL("Span: without wrapping it wants as many cells as it has characters") {
    const ui::Span one{"kot"};

    const ui::Size wants = one.wants({80, 25});

    same(wants.wide, 3);
    same(wants.high, 1);
}

TRIAL("Span: an empty one wants nothing, not even a row") {
    const ui::Span one{""};

    const ui::Size wants = one.wants({80, 25});

    same(wants.wide, 0);
    same(wants.high, 0);
}

TRIAL("Span: letters with diacritics count as characters, not bytes") {
    const ui::Span one{"żółw"};

    same(one.wants({80, 25}).wide, 4);
}

TRIAL("Span: a double-width character counts as two") {
    const ui::Span one{"漢字"};

    same(one.wants({80, 25}).wide, 4);
}

TRIAL("Span: without wrapping it ignores the available room") {
    const ui::Span one{"bardzo długi napis"};

    const ui::Size wants = one.wants({5, 25});

    same(wants.wide, 18);
    same(wants.high, 1);
}

TRIAL("Span: with wrapping it breaks at spaces") {
    ui::Span one{"ala ma kota", ui::SpanShape{true}};

    const ui::Size wants = one.wants({7, 25});

    same(wants.high, 2);
    same(wants.wide, 6);

    const Snapshot made = one.render({{0, 0}, {7, 2}});

    same(read(made, 0), std::string{"ala ma"});
    same(read(made, 1), std::string{"kota"});
}

TRIAL("Span: wrapping switches on the fly") {
    ui::Span one{"ala ma kota"};

    same(one.wants({7, 25}).high, 1);

    one.wrapped(true);

    same(one.wants({7, 25}).high, 2);
    ought(one.wrapped());
}

TRIAL("Span: a word longer than the row breaks where it stands") {
    ui::Span one{"nieprzewidywalnie", ui::SpanShape{true}};

    const Snapshot made = one.render({{0, 0}, {5, 4}});

    same(read(made, 0), std::string{"niepr"});
    same(read(made, 1), std::string{"zewid"});
    same(read(made, 2), std::string{"ywaln"});
    same(read(made, 3), std::string{"ie"});
}

TRIAL("Span: a long word does not swallow a row that already stands") {
    ui::Span one{"do nieprzewidywalnie", ui::SpanShape{true}};

    const Snapshot made = one.render({{0, 0}, {5, 5}});

    same(read(made, 0), std::string{"do"});
    same(read(made, 1), std::string{"niepr"});
}

TRIAL("Span: breaking counts widths, not bytes") {
    ui::Span one{"żółw żółw", ui::SpanShape{true}};

    same(one.wants({9, 25}).high, 1);
    same(one.wants({8, 25}).high, 2);
}

TRIAL("Span: double-width characters also break by cells, not by characters") {
    ui::Span one{"漢字 漢字", ui::SpanShape{true}};

    same(one.wants({9, 25}).high, 1);
    same(one.wants({8, 25}).high, 2);
}

TRIAL("Span: a run of spaces makes no empty rows") {
    ui::Span one{"ala   ma", ui::SpanShape{true}};

    same(one.wants({3, 25}).high, 2);
}

TRIAL("Span: zero width does not hang the breaking") {
    const ui::Span one{"cokolwiek", ui::SpanShape{true}};

    same(one.wants({0, 25}).high, 0);
}

TRIAL("Span: draws only as many rows as it was given") {
    ui::Span one{"ala ma kota i psa", ui::SpanShape{true}};

    const Snapshot made = one.render({{0, 0}, {6, 2}});

    same(read(made, 0), std::string{"ala ma"});
    same(read(made, 1), std::string{"kota i"});
}

TRIAL("Span: without wrapping the page cuts off what sticks out") {
    ui::Span one{"bardzo długi napis"};

    const Snapshot made = one.render({{0, 0}, {6, 1}});

    same(read(made, 0), std::string{"bardzo"});
}

TRIAL("Span: the text can be replaced and the measurement follows it") {
    ui::Span one{"kot"};

    same(one.text(), std::string{"kot"});
    same(one.wants({80, 25}).wide, 3);

    one.text("kotek");

    same(one.text(), std::string{"kotek"});
    same(one.wants({80, 25}).wide, 5);

    const Snapshot made = one.render({{0, 0}, {8, 1}});

    same(read(made, 0), std::string{"kotek"});
}

TRIAL("Span: replaced with an empty text it stops wanting anything") {
    ui::Span one{"kot"};

    one.text("");

    same(one.wants({80, 25}).high, 0);
}

// The color comes from the role in the skin, not from the span: `Span` has nothing to set.
TRIAL("Span: the role's color travels with the characters") {
    ui::Root top;

    const auto one = std::make_shared<ui::Span>("kot");

    top.children().add(one);

    Style told{};

    told.foreground = Color{false, 200, 100, 50};

    top.skin(ui::Skin{{{"talcite.normal", told}}});

    const Snapshot made = top.render({{0, 0}, {6, 1}});

    ought(!made.at({0, 0}).style.foreground.transparent);
    same(made.at({0, 0}).style.foreground.red, static_cast<std::uint8_t>(200));

    // Beyond the text there is nothing: the color travels with the characters, not the rectangle.
    ought(made.at({5, 0}).style.foreground.transparent);
}

TRIAL("Span: the built-in skin leaves the terminal's color") {
    ui::Span one{"kot"};

    const Snapshot made = one.render({{0, 0}, {6, 1}});

    ought(made.at({0, 0}).style.foreground.transparent);
}

TRIAL("Span: right alignment moves the text away from the left edge") {
    ui::Span one{"kot"};

    one.justify(ui::Justify::right);

    const Snapshot made = one.render({{0, 0}, {7, 1}});

    same(made.at({4, 0}).letter, U'k');
    same(made.at({0, 0}).letter, U' ');
    same(one.justify(), ui::Justify::right);
}

TRIAL("Span: centering splits the excess in half") {
    ui::Span one{"kot"};

    one.justify(ui::Justify::center);

    const Snapshot made = one.render({{0, 0}, {7, 1}});

    same(made.at({2, 0}).letter, U'k');
}

TRIAL("Span: alignment counts cells, not bytes") {
    ui::Span one{"żółw"};

    one.justify(ui::Justify::right);

    const Snapshot made = one.render({{0, 0}, {6, 1}});

    same(made.at({2, 0}).letter, U'ż');
}

TRIAL("Span: each wrapped row is aligned on its own") {
    ui::Span one{"ala ma kota", ui::SpanShape{true}};

    one.justify(ui::Justify::right);

    const Snapshot made = one.render({{0, 0}, {7, 2}});

    same(read(made, 0), std::string{" ala ma"});
    same(read(made, 1), std::string{"   kota"});
}

TRIAL("Span: without eliding the text is simply cut off") {
    ui::Span one{"nieprzewidywalnie"};

    const Snapshot made = one.render({{0, 0}, {6, 1}});

    same(read(made, 0), std::string{"nieprz"});
}

TRIAL("Span: eliding on the right keeps the start and an ellipsis") {
    ui::Span one{"nieprzewidywalnie"};

    one.elide(ui::Elide::right);

    const Snapshot made = one.render({{0, 0}, {6, 1}});

    same(read(made, 0), std::string{"niepr…"});
    same(one.elide(), ui::Elide::right);
}

TRIAL("Span: eliding on the left keeps the end") {
    ui::Span one{"nieprzewidywalnie"};

    one.elide(ui::Elide::left);

    const Snapshot made = one.render({{0, 0}, {6, 1}});

    same(read(made, 0), std::string{"…alnie"});
}

TRIAL("Span: eliding in the middle keeps both ends") {
    ui::Span one{"nieprzewidywalnie"};

    one.elide(ui::Elide::middle);

    const Snapshot made = one.render({{0, 0}, {7, 1}});

    same(read(made, 0), std::string{"nie…nie"});
}

TRIAL("Span: a text that fits is not elided") {
    ui::Span one{"kot"};

    one.elide(ui::Elide::right);

    const Snapshot made = one.render({{0, 0}, {6, 1}});

    same(read(made, 0), std::string{"kot"});
}

TRIAL("Span: a single cell is just the ellipsis") {
    ui::Span one{"kotek"};

    one.elide(ui::Elide::right);

    const Snapshot made = one.render({{0, 0}, {1, 1}});

    same(read(made, 0), std::string{"…"});
}

TRIAL("Span: eliding does not change what the text wants") {
    ui::Span one{"nieprzewidywalnie"};

    one.elide(ui::Elide::right);

    same(one.wants({6, 1}).wide, 17);
}

TRIAL("Span: eliding counts the cells of double-width characters") {
    ui::Span one{"漢字漢字"};

    one.elide(ui::Elide::right);

    const Snapshot made = one.render({{0, 0}, {5, 1}});

    same(read(made, 0), std::string{"漢字…"});
}

TRIAL("Span: in a tree it stands where its parent puts it") {
    ui::Column top;

    const auto first = std::make_shared<ui::Span>("pierwszy");
    const auto second = std::make_shared<ui::Span>("drugi");

    top.children().add(first);
    top.children().add(second);

    const Snapshot made = top.render({{0, 0}, {10, 3}});

    same(read(made, 0), std::string{"pierwszy"});
    same(read(made, 1), std::string{"drugi"});
    same(second->area().at.y, 1);
}

// --- Hard line ends ---------------------------------------------------------------------------
//
// A `\n` breaks the line instead of reaching the page as a byte that would come out as a
// replacement character in the middle of a sentence. Messages of two sentences are written this
// way, so the span has to understand them.

TRIAL("Span: a hard line end splits, also without wrapping") {
    const ui::Span one{"pierwszy\ndrugi"};

    const ui::Size wants = one.wants({80, 25});

    same(wants.wide, 8);
    same(wants.high, 2);

    ui::Span other{"pierwszy\ndrugi"};

    const Snapshot made = other.render({{0, 0}, {10, 3}});

    same(read(made, 0), std::string{"pierwszy"});
    same(read(made, 1), std::string{"drugi"});
}

TRIAL("Span: an empty paragraph stays an empty row") {
    ui::Span one{"nad\n\npod"};

    const Snapshot made = one.render({{0, 0}, {10, 4}});

    same(read(made, 0), std::string{"nad"});
    same(read(made, 1), std::string{});
    same(read(made, 2), std::string{"pod"});
    same(one.wants({80, 25}).high, 3);
}

TRIAL("Span: a line end at the end gives an empty row after it") {
    const ui::Span one{"sam\n"};

    same(one.wants({80, 25}).high, 2);
}

TRIAL("Span: a carriage return does not stay in the content") {
    ui::Span one{"pierwszy\r\ndrugi"};

    const Snapshot made = one.render({{0, 0}, {12, 2}});

    same(read(made, 0), std::string{"pierwszy"});
    same(read(made, 1), std::string{"drugi"});
    same(one.wants({80, 25}).wide, 8);
}

TRIAL("Span: each paragraph wraps on its own") {
    ui::Span one{"ala ma kota\nas", ui::SpanShape{true}};

    const Snapshot made = one.render({{0, 0}, {7, 4}});

    same(read(made, 0), std::string{"ala ma"});
    same(read(made, 1), std::string{"kota"});
    same(read(made, 2), std::string{"as"});
    same(one.wants({7, 25}).high, 3);
}

TRIAL("Span: alignment is computed for each hard line separately") {
    ui::Span one{"dluuugi\nkot", ui::SpanShape{false, ui::Justify::center}};

    const Snapshot made = one.render({{0, 0}, {9, 2}});

    same(read(made, 0), std::string{" dluuugi"});
    same(read(made, 1), std::string{"   kot"});
}


// A word longer than the row is cut into rows by walking along it once. Cutting it by erasing the
// head and measuring the rest again for every row costs the square of its length: a pasted address
// or a line of base64 in a message would freeze the screen.
TRIAL("Span: a very long word wraps in one pass") {
    ui::Span one{std::string(400000, 'x') + " koniec", ui::SpanShape{true}};

    const auto began = std::chrono::steady_clock::now();

    const ui::Size want = one.wants({40, 100});

    ought(std::chrono::steady_clock::now() - began < std::chrono::seconds{1});
    same(want.high, 10001);
    same(want.wide, 40);
}
