#include "tal/testing.hpp"

#include <string>
#include <vector>

#include "talcite/comb.hpp"

using namespace talcite;

namespace {

// The text of `where` that the hit covers.
std::string took(std::string_view where, Hit one) {
    return std::string{where.substr(one.from, one.upto - one.from)};
}

// The texts of all matches of `comb` in `where`, in order.
std::vector<std::string> every(const Comb& comb, std::string_view where) {
    std::vector<std::string> made;

    for (const Hit one : comb.all(where)) made.push_back(took(where, one));

    return made;
}

}

// --- What the comb knows about itself -----------------------------------------------------------

TRIAL("Comb: an empty one is not ready") {
    const Comb one;

    OUGHT(!one.ready());
    OUGHT(!one.wrong().empty());
    OUGHT(!one.forth("cokolwiek", 0).has_value());
}

TRIAL("Comb: an empty pattern is not ready") {
    const Comb one{"", {}};

    OUGHT(!one.ready());
}

// The pattern comes from a text field, so a bad pattern is an answer, not an exception.
TRIAL("Comb: a bad pattern does not throw, it says it is bad") {
    const Comb one{"[", Sift{true, false, false}};

    OUGHT(!one.ready());
    OUGHT(!one.wrong().empty());
    OUGHT(!one.forth("[abc]", 0).has_value());
}

TRIAL("Comb: remembers what it looks for") {
    const Comb one{"abc", Sift{false, true, false}};

    OUGHT(one.ready());
    SAME(one.what(), std::string{"abc"});
    OUGHT(one.how().cased);
    OUGHT(!one.how().regex);
}

// --- Plain search --------------------------------------------------------------------------------

TRIAL("Comb: finds the text and gives its range") {
    const Comb one{"drugi", {}};

    const std::optional<Hit> got = one.forth("pierwszy drugi trzeci", 0);

    OUGHT(got.has_value());
    SAME(got->from, std::size_t{9});
    SAME(got->upto, std::size_t{14});
}

TRIAL("Comb: searches from the given place") {
    const Comb one{"ab", {}};

    SAME(one.forth("abXab", 0)->from, std::size_t{0});
    SAME(one.forth("abXab", 1)->from, std::size_t{3});
    OUGHT(!one.forth("abXab", 4).has_value());
}

TRIAL("Comb: case-insensitive by default") {
    const Comb one{"ALA", {}};

    OUGHT(one.forth("mala ala", 0).has_value());
    SAME(one.forth("mala ala", 0)->from, std::size_t{1});
}

TRIAL("Comb: case-sensitive searches exactly") {
    const Comb one{"ALA", Sift{false, true, false}};

    OUGHT(!one.forth("mala ala", 0).has_value());
    OUGHT(one.forth("mala ALA", 0).has_value());
}

// Whole words: "ala" in "mala" is not a word, but "ala" in "ala." is.
TRIAL("Comb: whole words skip matches inside a word") {
    const Comb one{"ala", Sift{false, false, true}};

    SAME(one.forth("mala ala.", 0)->from, std::size_t{5});
    OUGHT(!one.forth("malaala", 0).has_value());
    SAME(one.forth("ala", 0)->from, std::size_t{0});
}

// Bytes outside ASCII count as letters, so "ala" followed by a multibyte letter is not a whole
// word.
TRIAL("Comb: a letter with a diacritic is a letter too") {
    const Comb one{"ala", Sift{false, false, true}};

    OUGHT(!one.forth("al\xc4\x85", 0).has_value());
    OUGHT(!one.forth("ala\xc4\x85", 0).has_value());
}

TRIAL("Comb: does not find what is not there") {
    const Comb one{"xyz", {}};

    OUGHT(!one.forth("abcdef", 0).has_value());
    OUGHT(!one.back("abcdef", 6).has_value());
    OUGHT(one.all("abcdef").empty());
}

TRIAL("Comb: a pattern longer than the line breaks nothing") {
    const Comb one{"abcdef", {}};

    OUGHT(!one.forth("abc", 0).has_value());
    OUGHT(!one.forth("", 0).has_value());
}

// --- Backwards and all ---------------------------------------------------------------------------

TRIAL("Comb: backwards takes the last match before the place") {
    const Comb one{"ab", {}};

    const std::string where = "ab.ab.ab";

    SAME(one.back(where, 8)->from, std::size_t{6});
    SAME(one.back(where, 6)->from, std::size_t{3});
    SAME(one.back(where, 3)->from, std::size_t{0});
    OUGHT(!one.back(where, 0).has_value());
}

TRIAL("Comb: all matches go from the left and do not overlap") {
    const Comb one{"aa", {}};

    const std::vector<std::string> want{"aa", "aa"};

    SAME(every(one, "aaaa"), want);
    SAME(every(one, "aaa"), std::vector<std::string>{"aa"});
}

// --- Regular expressions -------------------------------------------------------------------------

TRIAL("Comb: a regular expression finds the pattern") {
    const Comb one{"[0-9]+", Sift{true, false, false}};

    const std::string where = "rok 2026 i 12 miesiecy";

    const std::vector<std::string> want{"2026", "12"};

    SAME(took(where, *one.forth(where, 0)), std::string{"2026"});
    SAME(every(one, where), want);
}

TRIAL("Comb: a case-insensitive regular expression") {
    const Comb one{"ala", Sift{true, false, false}};

    OUGHT(one.forth("ALA", 0).has_value());

    const Comb two{"ala", Sift{true, true, false}};

    OUGHT(!two.forth("ALA", 0).has_value());
}

// Searching from the middle of a line must not pretend that the line starts at that place.
TRIAL("Comb: the caret does not match in the middle of a line") {
    const Comb one{"^ab", Sift{true, false, false}};

    SAME(one.forth("abXab", 0)->from, std::size_t{0});
    OUGHT(!one.forth("abXab", 1).has_value());
}

TRIAL("Comb: a word boundary sees the byte before it") {
    const Comb one{"\\bab", Sift{true, false, false}};

    const std::string where = "xab ab";

    SAME(one.forth(where, 0)->from, std::size_t{4});
    SAME(one.forth(where, 2)->from, std::size_t{4});
}

// An empty match is skipped: replacing emptiness makes no sense, and repeated searching would
// stand still.
TRIAL("Comb: an empty match does not count") {
    const Comb one{"x*", Sift{true, false, false}};

    OUGHT(!one.forth("abc", 0).has_value());
    SAME(every(one, "axxb"), std::vector<std::string>{"xx"});
}

TRIAL("Comb: whole words work with an expression too") {
    const Comb one{"al.", Sift{true, false, true}};

    SAME(one.forth("malaz ala ", 0)->from, std::size_t{6});
}
