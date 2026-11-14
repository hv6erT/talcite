#include <cstddef>
#include <string>

#include "tal/editing.hpp"
#include "tal/text.hpp"

#include "lining.hpp"
#include "tal/testing.hpp"

// The core of editing one line, which a field and an editor share: where a word starts and ends,
// a cluster at a time, and which steps of the history join.

namespace {

using tal::Place;
using tal::lining::Doing;
using tal::lining::Piece;
using tal::lining::joined;
using tal::lining::worded;
using tal::text::Rendering;

constexpr Rendering clustered{true, true};
constexpr Rendering loose{false, true};

}

// A word is made of letters, digits and the underscore, and of every character past ASCII; a
// cluster counts by its first character, and past the end of the line there is no word.
TRIAL("lining: what starts a word") {
    using tal::lining::lettered;

    OUGHT(lettered("a", 0));
    OUGHT(lettered("Z9_", 1));
    OUGHT(lettered("Z9_", 2));
    OUGHT(!lettered("a b", 1));
    OUGHT(!lettered("a,b", 1));
    OUGHT(lettered("żółw", 0));
    OUGHT(lettered("ab", 1));
    OUGHT(!lettered("ab", 2));
    OUGHT(!lettered("", 0));
}

// Forth goes past what is not a word, then to the end of the word; back to the start of the word
// before. The ends of the line stop both, and a place past the end counts as the end.
TRIAL("lining: words forth and back") {
    const std::string line = "ab, cd  ef";

    SAME(worded(line, 0, true, clustered), std::size_t{2});
    SAME(worded(line, 2, true, clustered), std::size_t{6});
    SAME(worded(line, 3, true, clustered), std::size_t{6});
    SAME(worded(line, 6, true, clustered), std::size_t{10});
    SAME(worded(line, 10, true, clustered), std::size_t{10});
    SAME(worded(line, 99, true, clustered), std::size_t{10});
    SAME(worded(line, 10, false, clustered), std::size_t{8});
    SAME(worded(line, 8, false, clustered), std::size_t{4});
    SAME(worded(line, 5, false, clustered), std::size_t{4});
    SAME(worded(line, 2, false, clustered), std::size_t{0});
    SAME(worded(line, 0, false, clustered), std::size_t{0});
    SAME(worded(line, 99, false, clustered), std::size_t{8});
    SAME(worded("  ", 0, true, clustered), std::size_t{2});
    SAME(worded("  ", 2, false, clustered), std::size_t{0});
    SAME(worded("", 0, true, clustered), std::size_t{0});

    const std::string polish = "zażółć gęślą";

    SAME(worded(polish, 0, true, clustered), std::string{"zażółć"}.size());
    SAME(worded(polish, polish.size(), false, clustered), std::string{"zażółć "}.size());
}

// A combining mark joins the space before it in either rendering, so that cluster is no word; an
// emoji joined or loose is a word of characters past ASCII, so it does not stop one. A modifier
// after a space joins it only when joined: then that cluster is no word, loose it is a word.
TRIAL("lining: words over a combining mark and an emoji, joined and loose") {
    const std::string line = "ab \u0301cd";

    SAME(worded(line, 2, true, clustered), line.size());
    SAME(worded(line, 2, true, loose), line.size());
    SAME(worded(line, line.size(), false, clustered), std::size_t{5});
    SAME(worded(line, line.size(), false, loose), std::size_t{5});

    const std::string family = "x \U0001F468\u200D\U0001F469\U0001F3FBy z";

    SAME(worded(family, 1, true, clustered), family.size() - 2);
    SAME(worded(family, 1, true, loose), family.size() - 2);
    SAME(worded(family, family.size() - 2, false, clustered), std::size_t{2});
    SAME(worded(family, family.size() - 2, false, loose), std::size_t{2});

    const std::string toned = "ab \U0001F3FBcd";

    SAME(worded(toned, toned.size(), false, clustered), std::size_t{7});
    SAME(worded(toned, toned.size(), false, loose), std::size_t{3});

    const std::string alone = " \U0001F3FB x";

    SAME(worded(alone, 0, true, clustered), alone.size());
    SAME(worded(alone, 0, true, loose), std::size_t{5});
}

// Where a piece of text ends: in one line by its bytes, in a text by its lines.
TRIAL("lining: where a piece ends") {
    using tal::lining::ended;

    SAME(ended(std::size_t{3}, "abc"), std::size_t{6});
    SAME(ended(std::size_t{3}, ""), std::size_t{3});
    OUGHT(ended(Place{2, 5}, "xy") == (Place{2, 7}));
    OUGHT(ended(Place{2, 5}, "x\ny\nzw") == (Place{4, 2}));
    OUGHT(ended(Place{2, 5}, "x\n") == (Place{3, 0}));
}

// Typing joins where the last typing ended, without a space, a tab or a line end, and not over a
// selection; its text grows.
TRIAL("lining: typing joins") {
    Piece<std::size_t> last{2, {}, "ab"};

    OUGHT(joined(Doing::typing, last, Doing::typing, Piece<std::size_t>{4, {}, "c"}));
    SAME(last.came, std::string{"abc"});
    SAME(last.from, std::size_t{2});
    OUGHT(!joined(Doing::typing, last, Doing::typing, Piece<std::size_t>{4, {}, "d"}));
    OUGHT(!joined(Doing::typing, last, Doing::typing, Piece<std::size_t>{5, {}, " "}));
    OUGHT(!joined(Doing::typing, last, Doing::typing, Piece<std::size_t>{5, {}, "\t"}));
    OUGHT(!joined(Doing::typing, last, Doing::typing, Piece<std::size_t>{5, {}, "\n"}));
    OUGHT(!joined(Doing::typing, last, Doing::typing, Piece<std::size_t>{5, "x", "d"}));
    OUGHT(!joined(Doing::typing, last, Doing::back, Piece<std::size_t>{4, "c", {}}));
    SAME(last.came, std::string{"abc"});

    Piece<std::size_t> spaced{0, "sel", " a"};

    OUGHT(joined(Doing::typing, spaced, Doing::typing, Piece<std::size_t>{2, {}, "b"}));
    SAME(spaced.came, std::string{" ab"});
    SAME(spaced.gone, std::string{"sel"});

    Piece<Place> broken{Place{0, 1}, {}, "x\n"};

    OUGHT(joined(Doing::typing, broken, Doing::typing, Piece<Place>{Place{1, 0}, {}, "y"}));
    SAME(broken.came, std::string{"x\ny"});
}

// Erasing back joins the character just before what went, and the piece starts there; erasing
// forth joins at the same place, and what goes grows after it. Neither joins across a line end.
TRIAL("lining: erasing back and forth joins") {
    Piece<std::size_t> back{4, "c", {}};

    OUGHT(joined(Doing::back, back, Doing::back, Piece<std::size_t>{3, "b", {}}));
    SAME(back.from, std::size_t{3});
    SAME(back.gone, std::string{"bc"});
    OUGHT(!joined(Doing::back, back, Doing::back, Piece<std::size_t>{1, "a", {}}));
    OUGHT(!joined(Doing::back, back, Doing::forth, Piece<std::size_t>{3, "x", {}}));
    OUGHT(!joined(Doing::back, back, Doing::back, Piece<std::size_t>{2, "\n", {}}));

    Piece<std::size_t> forth{1, "a", {}};

    OUGHT(joined(Doing::forth, forth, Doing::forth, Piece<std::size_t>{1, "b", {}}));
    SAME(forth.gone, std::string{"ab"});
    SAME(forth.from, std::size_t{1});
    OUGHT(!joined(Doing::forth, forth, Doing::forth, Piece<std::size_t>{2, "c", {}}));
    OUGHT(!joined(Doing::forth, forth, Doing::forth, Piece<std::size_t>{1, "\n", {}}));
    OUGHT(!joined(Doing::forth, forth, Doing::back, Piece<std::size_t>{0, "x", {}}));

    Piece<Place> lined{Place{1, 0}, "y", {}};

    OUGHT(!joined(Doing::back, lined, Doing::back, Piece<Place>{Place{0, 3}, "\n", {}}));

    Piece<std::size_t> other{0, "a", "b"};

    OUGHT(!joined(Doing::other, other, Doing::other, Piece<std::size_t>{1, {}, "c"}));
}
