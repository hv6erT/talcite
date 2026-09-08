#include "proving.hpp"

#include <string>
#include <vector>

#include "talcite/comb.hpp"

using namespace talcite;

namespace {

std::string took(std::string_view where, Hit one) {
    return std::string{where.substr(one.from, one.upto - one.from)};
}

std::vector<std::string> every(const Comb& comb, std::string_view where) {
    std::vector<std::string> made;

    for (const Hit one : comb.all(where)) made.push_back(took(where, one));

    return made;
}

}

// --- Co grzebień wie o sobie ---------------------------------------------------------------------

TRIAL("Comb: pusty nie jest gotowy") {
    const Comb one;

    ought(!one.ready());
    ought(!one.wrong().empty());
    ought(!one.forth("cokolwiek", 0).has_value());
}

TRIAL("Comb: pusty wzorzec nie jest gotowy") {
    const Comb one{"", {}};

    ought(!one.ready());
}

// Wzorzec przychodzi z pola tekstowego, więc zły wzorzec to odpowiedź, a nie wyjątek.
TRIAL("Comb: zły wzorzec nie rzuca, tylko mówi, że jest zły") {
    const Comb one{"[", Sift{true, false, false}};

    ought(!one.ready());
    ought(!one.wrong().empty());
    ought(!one.forth("[abc]", 0).has_value());
}

TRIAL("Comb: pamięta, czego szuka") {
    const Comb one{"abc", Sift{false, true, false}};

    ought(one.ready());
    same(one.what(), std::string{"abc"});
    ought(one.how().cased);
    ought(!one.how().regex);
}

// --- Zwykłe szukanie -----------------------------------------------------------------------------

TRIAL("Comb: znajduje napis i oddaje jego zakres") {
    const Comb one{"drugi", {}};

    const std::optional<Hit> got = one.forth("pierwszy drugi trzeci", 0);

    ought(got.has_value());
    same(got->from, std::size_t{9});
    same(got->upto, std::size_t{14});
}

TRIAL("Comb: szuka od podanego miejsca") {
    const Comb one{"ab", {}};

    same(one.forth("abXab", 0)->from, std::size_t{0});
    same(one.forth("abXab", 1)->from, std::size_t{3});
    ought(!one.forth("abXab", 4).has_value());
}

TRIAL("Comb: bez wielkości liter domyślnie") {
    const Comb one{"ALA", {}};

    ought(one.forth("mala ala", 0).has_value());
    same(one.forth("mala ala", 0)->from, std::size_t{1});
}

TRIAL("Comb: z wielkością liter szuka dokładnie") {
    const Comb one{"ALA", Sift{false, true, false}};

    ought(!one.forth("mala ala", 0).has_value());
    ought(one.forth("mala ALA", 0).has_value());
}

// Całe wyrazy: „ala" w „mala" to nie wyraz, a „ala" w „ala." już tak.
TRIAL("Comb: całe wyrazy pomijają dopasowania w środku wyrazu") {
    const Comb one{"ala", Sift{false, false, true}};

    same(one.forth("mala ala.", 0)->from, std::size_t{5});
    ought(!one.forth("malaala", 0).has_value());
    same(one.forth("ala", 0)->from, std::size_t{0});
}

// Bajty spoza ASCII liczymy jako literę, więc „ala" w „alą" nie jest całym wyrazem.
TRIAL("Comb: ogonek też jest literą") {
    const Comb one{"ala", Sift{false, false, true}};

    ought(!one.forth("al\xc4\x85", 0).has_value());
    ought(!one.forth("ala\xc4\x85", 0).has_value());
}

TRIAL("Comb: nie znajduje, czego nie ma") {
    const Comb one{"xyz", {}};

    ought(!one.forth("abcdef", 0).has_value());
    ought(!one.back("abcdef", 6).has_value());
    ought(one.all("abcdef").empty());
}

TRIAL("Comb: dłuższy wzorzec niż wiersz nie psuje niczego") {
    const Comb one{"abcdef", {}};

    ought(!one.forth("abc", 0).has_value());
    ought(!one.forth("", 0).has_value());
}

// --- Wstecz i wszystkie --------------------------------------------------------------------------

TRIAL("Comb: wstecz bierze ostatnie dopasowanie przed miejscem") {
    const Comb one{"ab", {}};

    const std::string where = "ab.ab.ab";

    same(one.back(where, 8)->from, std::size_t{6});
    same(one.back(where, 6)->from, std::size_t{3});
    same(one.back(where, 3)->from, std::size_t{0});
    ought(!one.back(where, 0).has_value());
}

TRIAL("Comb: wszystkie dopasowania idą od lewej i się nie zachodzą") {
    const Comb one{"aa", {}};

    const std::vector<std::string> want{"aa", "aa"};

    same(every(one, "aaaa"), want);
    same(every(one, "aaa"), std::vector<std::string>{"aa"});
}

// --- Wyrażenia regularne -------------------------------------------------------------------------

TRIAL("Comb: wyrażenie regularne znajduje wzór") {
    const Comb one{"[0-9]+", Sift{true, false, false}};

    const std::string where = "rok 2026 i 12 miesiecy";

    const std::vector<std::string> want{"2026", "12"};

    same(took(where, *one.forth(where, 0)), std::string{"2026"});
    same(every(one, where), want);
}

TRIAL("Comb: wyrażenie regularne bez wielkości liter") {
    const Comb one{"ala", Sift{true, false, false}};

    ought(one.forth("ALA", 0).has_value());

    const Comb two{"ala", Sift{true, true, false}};

    ought(!two.forth("ALA", 0).has_value());
}

// Szukanie od środka wiersza nie może udawać, że wiersz zaczyna się w tym miejscu.
TRIAL("Comb: daszek nie łapie się w środku wiersza") {
    const Comb one{"^ab", Sift{true, false, false}};

    same(one.forth("abXab", 0)->from, std::size_t{0});
    ought(!one.forth("abXab", 1).has_value());
}

TRIAL("Comb: granica wyrazu widzi bajt przed sobą") {
    const Comb one{"\\bab", Sift{true, false, false}};

    const std::string where = "xab ab";

    same(one.forth(where, 0)->from, std::size_t{4});
    same(one.forth(where, 2)->from, std::size_t{4});
}

// Puste dopasowanie pomijamy: zamiana pustki nie ma sensu, a szukanie w kółko stałoby w miejscu.
TRIAL("Comb: puste dopasowanie się nie liczy") {
    const Comb one{"x*", Sift{true, false, false}};

    ought(!one.forth("abc", 0).has_value());
    same(every(one, "axxb"), std::vector<std::string>{"xx"});
}

TRIAL("Comb: całe wyrazy działają też przy wyrażeniu") {
    const Comb one{"al.", Sift{true, false, true}};

    same(one.forth("malaz ala ", 0)->from, std::size_t{6});
}
