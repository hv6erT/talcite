#include "proving.hpp"

#include <cstddef>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "faking.hpp"
#include "talcite/bytes.hpp"
#include "talcite/editor.hpp"
#include "talcite/encoding.hpp"
#include "talcite/field.hpp"
#include "talcite/root.hpp"
#include "talcite/term.hpp"
#include "talcite/view.hpp"

#include "conhost.hpp"

using namespace talcite;

// Characters as a terminal draws them: clusters, their widths, the glyphs that stand for them in a
// cell, and the editing that takes them whole. The rendering is process-wide; a trial that changes
// it puts the default back with `Rendered`, so the order of trials does not matter.

namespace {

// Sets a rendering for the lifetime of the object, then the previous one again.
class Rendered {
public:
    // Sets `how` until the end of the scope.
    explicit Rendered(Rendering how) : was_(rendering()) { rendered(how); }

    // Puts the previous rendering back.
    ~Rendered() { rendered(was_); }

    Rendered(const Rendered&) = delete;
    Rendered& operator=(const Rendered&) = delete;
    Rendered(Rendered&&) = delete;
    Rendered& operator=(Rendered&&) = delete;

private:
    Rendering was_;
};

// The text cut into clusters, separated by "|".
std::string parted(std::string_view text) {
    std::string made;

    for (std::size_t at = 0; at < text.size();) {
        const std::size_t next = cluster_after(text, at);

        if (!made.empty()) made += '|';

        made += text.substr(at, next - at);
        at = next;
    }

    return made;
}

// The starts of the clusters walking back from the end, as `cluster_before` finds them.
std::vector<std::size_t> backward(std::string_view text) {
    std::vector<std::size_t> made;

    for (std::size_t at = text.size(); at > 0;) {
        at = cluster_before(text, at);

        made.insert(made.begin(), at);
    }

    return made;
}

// The starts of the clusters walking forward.
std::vector<std::size_t> forward(std::string_view text) {
    std::vector<std::size_t> made;

    for (std::size_t at = 0; at < text.size(); at = cluster_after(text, at)) made.push_back(at);

    return made;
}

// One row of a view as text, trailing spaces removed.
std::string row(const View& one, int y) {
    std::string made;

    for (int x = 0; x < one.size().wide; ++x)
        if (const char32_t letter = one.at({x, y}).letter; letter != 0) made += spelled(letter);

    while (!made.empty() && made.back() == ' ') made.pop_back();

    return made;
}

const std::string acute = "\xcc\x81";
const std::string zwj = "\xe2\x80\x8d";
const std::string woman = "\xf0\x9f\x91\xa9";
const std::string laptop = "\xf0\x9f\x92\xbb";
const std::string thumb = "\xf0\x9f\x91\x8d";
const std::string tone = "\xf0\x9f\x8f\xbd";
const std::string heart = "\xe2\x9d\xa4";
const std::string selector = "\xef\xb8\x8f";
const std::string poland = "\xf0\x9f\x87\xb5\xf0\x9f\x87\xb1";

}

// --- Widths of single characters -----------------------------------------------------------------

TRIAL("Clusters: the width of a single character") {
    const Rendered joined{Rendering{true, true}};

    same(width(U'a'), 1);
    same(width(U'\x142'), 1);
    same(width(U'\x4F60'), 2);
    same(width(U'\x301'), 0);
    same(width(U'\x200B'), 0);
    same(width(U'\x200D'), 0);
    same(width(U'\xFE0F'), 0);
    same(width(U'\x1161'), 0);
    same(width(U'\x1100'), 2);
    same(width(U'\x01'), 1);
    same(width(U'\x202E'), 1);
    same(width(U'\U0001F469'), 2);
    same(width(U'\x231A'), 2);
    same(width(U'\U0001F3FD'), 0);

    const Rendered apart{Rendering{false, false}};

    same(width(U'\U0001F3FD'), 2);
}

// --- Boundaries ------------------------------------------------------------------------------------

// A letter keeps its accents, whatever came before them, and a mark after a control has nothing to
// sit on.
TRIAL("Clusters: letters with their marks, as names from macOS have them") {
    same(parted("Zoe" + acute), std::string{"Z|o|e" + acute});
    same(parted("o" + acute + "\xcc\xa8" + "x"), std::string{"o" + acute + "\xcc\xa8|x"});
    same(parted(acute + "a"), std::string{acute + "|a"});
    same(parted("\x01" + acute), std::string{"\x01|" + acute});
    same(parted("a\r\nb"), std::string{"a|\r|\n|b"});
}

// Korean from macOS comes as jamo: an initial, a vowel and a final make one syllable.
TRIAL("Clusters: Hangul jamo make syllables") {
    const std::string han = "\xe1\x84\x92\xe1\x85\xa1\xe1\x86\xab";
    const std::string geul = "\xe1\x84\x80\xe1\x85\xb3\xe1\x86\xaf";

    same(parted(han + geul), han + "|" + geul);
    same(width(han + geul), 4);
    same(parted("\xea\xb0\x80\xe1\x86\xa8"), std::string{"\xea\xb0\x80\xe1\x86\xa8"});
}

// Regional indicators pair up from the left: three make a flag and a lone half.
TRIAL("Clusters: flags are pairs of regional indicators") {
    same(parted(poland + "\xf0\x9f\x87\xa9"), poland + "|\xf0\x9f\x87\xa9");
    same(width(poland), 2);
    same(width(poland + "\xf0\x9f\x87\xa9"), 3);
}

// A terminal that joins draws a programmer and a dark thumb in two cells; one that does not draws
// a woman and a laptop, a thumb and a colour, each in two.
TRIAL("Clusters: emoji sequences follow the terminal") {
    {
        const Rendered joined{Rendering{true, true}};

        same(parted(woman + zwj + laptop + "!"), woman + zwj + laptop + "|!");
        same(width(woman + zwj + laptop), 2);
        same(parted(thumb + tone), thumb + tone);
        same(width(thumb + tone), 2);
        same(width(heart + selector), 2);
    }

    {
        const Rendered apart{Rendering{false, false}};

        same(parted(woman + zwj + laptop + "!"), woman + zwj + "|" + laptop + "|!");
        same(width(woman + zwj + laptop), 4);
        same(parted(thumb + tone), thumb + "|" + tone);
        same(width(thumb + tone), 4);
        same(width(heart + selector), 1);
    }
}

TRIAL("Clusters: a format character alone shows nothing, a mark alone takes a cell") {
    same(width("a\xe2\x80\x8b" "b"), 2);
    same(width(acute), 1);
    same(width("\xe2\x80\x8d"), 0);
}

// Walking back finds the same boundaries as walking forward, for strings mixed from every kind of
// character, in both renderings.
TRIAL("Clusters: walking back agrees with walking forward") {
    const std::vector<std::string> pieces{"a", "\xc5\x82", acute, "\xcc\xa8", zwj, woman, laptop, thumb, tone,
                                          heart, selector, "\xf0\x9f\x87\xb5", "\xf0\x9f\x87\xb1", "\xe4\xbd\xa0",
                                          "\xe1\x84\x92", "\xe1\x85\xa1", "\xe1\x86\xab", "\xea\xb0\x80", "\r", "\n",
                                          "\xe2\x80\x8b", " "};

    std::mt19937 random{20261013};

    for (const Rendering how : {Rendering{true, true}, Rendering{false, false}}) {
        const Rendered set{how};

        for (int round = 0; round < 3000; ++round) {
            std::string text;

            const int many = 1 + static_cast<int>(random() % 12);

            for (int one = 0; one < many; ++one) text += pieces[random() % pieces.size()];

            if (backward(text) != forward(text)) {
                ought(false);

                return;
            }
        }
    }
}

// --- Glyphs ----------------------------------------------------------------------------------------

TRIAL("Clusters: a glyph is the character itself, or a handle that gives the cluster back") {
    same(static_cast<std::uint32_t>(glyph("a")), static_cast<std::uint32_t>(U'a'));
    same(static_cast<std::uint32_t>(glyph("\x01")), static_cast<std::uint32_t>(U'\x2401'));

    const char32_t accented = glyph("e" + acute);

    ought(accented >= 0x110000);
    same(spelled(accented), std::string{"e" + acute});
    same(static_cast<std::uint32_t>(glyph("e" + acute)), static_cast<std::uint32_t>(accented));
    same(width(accented), 1);

    // A mark alone stands on a dotted circle; a format character alone is no glyph at all.
    same(spelled(glyph(acute)), std::string{"\xe2\x97\x8c" + acute});
    same(static_cast<std::uint32_t>(glyph("\xe2\x80\x8b")), 0u);
    same(spelled(0), std::string{});
}

// The same cluster is wider on a terminal that presents the selector, so it gets its own handle.
TRIAL("Clusters: a glyph remembers the rendering it was measured in") {
    char32_t wide = 0;
    char32_t narrow = 0;

    {
        const Rendered presented{Rendering{true, true}};

        wide = glyph(heart + selector);
    }

    {
        const Rendered plain{Rendering{true, false}};

        narrow = glyph(heart + selector);
    }

    ought(wide != narrow);
    same(width(wide), 2);
    same(width(narrow), 1);
}

// --- Drawing ---------------------------------------------------------------------------------------

TRIAL("Clusters: a view puts a letter with its accent in one cell") {
    View one{{10, 1}};

    one.put({0, 0}, "Zoe" + acute + "!", Style{});

    same(row(one, 0), std::string{"Zoe" + acute + "!"});
    same(spelled(one.at({2, 0}).letter), std::string{"e" + acute});
    same(static_cast<std::uint32_t>(one.at({3, 0}).letter), static_cast<std::uint32_t>(U'!'));
    same(measured("Zoe" + acute + "!").wide, 4);
}

// Code that puts text character by character and moves by `width` still gets one letter: the mark
// joins the glyph on its left, past the second half of a wide one.
TRIAL("Clusters: a mark put alone joins the glyph on its left") {
    View one{{10, 1}};

    one.put({0, 0}, U'e', Style{});
    one.put({1, 0}, U'\x301', Style{});
    one.put({1, 0}, U'\x4F60', Style{});
    one.put({3, 0}, U'\x301', Style{});

    same(spelled(one.at({0, 0}).letter), std::string{"e" + acute});
    same(spelled(one.at({1, 0}).letter), std::string{"\xe4\xbd\xa0" + acute});

    View empty{{3, 1}};

    empty.put({0, 0}, U'\x301', Style{});

    same(row(empty, 0), std::string{});
}

namespace {

// A root that writes a letter with its accent and a letter after it.
class Lettered : public ui::Root {
protected:
    // Puts the text at the top left of an otherwise empty page.
    View paint(ui::Size given, ui::Area) override {
        View made{given};

        made.put({0, 0}, "e" + acute + "x", Style{});

        return made;
    }
};

}

// The terminal gets a cluster as one character: the letter and its accent go out together, and
// the next cell follows without a jump.
TRIAL("Clusters: Term writes a cluster whole") {
    auto line = faking::terminal();

    Lettered root;

    Term term{Setup{}, Seam<Console>{line}};

    term.show(root);

    line->written.clear();

    term.draw();

    ought(line->written.find("e" + acute + "x") != std::string::npos);
}

// The probe's widths decide the rendering: a programmer four columns wide means the terminal does
// not join, a heart with the selector two columns wide means it presents. The rendering goes back
// when the terminal does.
TRIAL("Clusters: Term takes the rendering from the probe and gives it back") {
    const Rendered start{Rendering{true, false}};

    {
        auto line = faking::terminal("\x1b[1;3R\x1b[1;3R\x1b[1;5R\x1b[1;2R\x1b[1;2R\x1b[?62;1c");

        const Term term{Setup{}, Seam<Console>{line}};

        same(rendering().joined, false);
        same(rendering().presented, true);
    }

    same(rendering().joined, true);
    same(rendering().presented, false);
}

// --- Editing ---------------------------------------------------------------------------------------

namespace {

class Sheet : public ui::Root {};

}

// Backspace and Delete take the whole character, as the screen shows it, and the arrows step over
// it: "e" with U+0301 from a name made on a Mac, and a programmer on a terminal that joins.
TRIAL("Clusters: the editor takes a character whole") {
    const Rendered joined{Rendering{true, true}};

    Sheet sheet;

    const auto one = std::make_shared<ui::Editor>(Seam<Clipboard>{faking::clipboard()});

    one->hold(bytes("Zoe" + acute + woman + zwj + laptop + "!"));
    sheet.children().add(one);
    one->focus();

    one->cursor(std::string{"Zoe" + acute + woman + zwj + laptop}.size());

    ui::KeyEvent back{ui::Code::backspace};

    ought(sheet.dispatch(back));
    same(one->text(), std::string{"Zoe" + acute + "!"});

    ui::KeyEvent again{ui::Code::backspace};

    ought(sheet.dispatch(again));
    same(one->text(), std::string{"Zo!"});

    one->hold(bytes("Zoe" + acute + "!"));
    one->cursor(2);

    ui::KeyEvent remove{ui::Code::remove};

    ought(sheet.dispatch(remove));
    same(one->text(), std::string{"Zo!"});

    one->hold(bytes("e" + acute + "x"));
    one->cursor(0);

    ui::KeyEvent right{ui::Code::right};

    ought(sheet.dispatch(right));
    same(one->cursor(), std::size_t{3});
}

TRIAL("Clusters: a field takes a character whole and counts it as one") {
    ui::Root top;

    const auto one = std::make_shared<ui::Field>(10, "Zoe" + acute);

    top.children().add(one);
    one->focus();

    same(one->cursor(), std::size_t{3});

    ui::KeyEvent back{ui::Code::backspace};

    ought(top.dispatch(back));
    same(one->text(), std::string{"Zo"});
}

// --- The old Windows console ------------------------------------------------------------------------

// The old console's cell holds one character, so the screen keeps the cluster in one cell, and the
// glue sends its composed form; the cursor does not move for the accent.
TRIAL("Clusters: the old console's screen keeps a cluster in one cell") {
    conhost::Screen one{{10, 2}, 7, 0};

    one.fed("e" + acute + "x");

    same(spelled(one.at(0, 0).glyph), std::string{"e" + acute});
    same(static_cast<std::uint32_t>(one.at(1, 0).glyph), static_cast<std::uint32_t>(U'x'));
    same(one.cursor().x, 2);
}
