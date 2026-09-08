#include "proving.hpp"

#include <string>
#include <vector>

#include "talcite/bytes.hpp"

#include "spool.hpp"

using namespace talcite;

namespace {

Bytes lump(const std::string& what) {
    Bytes made;

    made.reserve(what.size());

    for (const char sign : what) made.push_back(static_cast<std::byte>(sign));

    return made;
}

// Źródło strumieniowe, które **liczy wołania**. Na nim widać, czy okno bajtów naprawdę oszczędza:
// przewijanie po sąsiedztwie nie ma prawa schodzić do niego przy każdym wierszu.
class Taped {
public:
    explicit Taped(std::string what) : raw_(lump(what)) {}

    [[nodiscard]] std::size_t whole() const { return raw_.size(); }

    [[nodiscard]] int calls() const { return calls_; }

    [[nodiscard]] Feed feed() {
        return [this](std::size_t at, std::size_t most) {
            ++calls_;

            Bytes made;

            for (std::size_t one = at; one < raw_.size() && one < at + most; ++one)
                made.push_back(raw_[one]);

            return made;
        };
    }

private:
    Bytes raw_;
    int calls_ = 0;
};

std::string many(int much, const char* said = "wiersz ") {
    std::string made;

    for (int one = 0; one < much; ++one) made += said + std::to_string(one) + "\n";

    return made;
}

}

// --- Dokument w pamięci --------------------------------------------------------------------------

TRIAL("Spool: liczy wiersze przy podaniu") {
    Spool one{lump("a\nbb\nccc")};

    ought(one.counted().has_value());
    same(*one.counted(), std::size_t{3});
    same(one.whole(), std::size_t{8});
}

TRIAL("Spool: oddaje treść bez końca wiersza") {
    Spool one{lump("a\nbb\nccc")};

    const std::vector<std::string> got = one.lines(0, 10);

    same(got.size(), std::size_t{3});
    same(got[0], std::string{"a"});
    same(got[1], std::string{"bb"});
    same(got[2], std::string{"ccc"});
}

TRIAL("Spool: zna bajt początku każdego wiersza") {
    Spool one{lump("a\nbb\nccc")};

    same(*one.at(0), std::size_t{0});
    same(*one.at(1), std::size_t{2});
    same(*one.at(2), std::size_t{5});
    ought(!one.at(3).has_value());
}

// Koniec wiersza na końcu pliku **nie robi** pustego wiersza po sobie. Inaczej każdy porządnie
// zakończony plik miałby o jeden wiersz za dużo, a taki jest niemal każdy.
TRIAL("Spool: enter na końcu nie dokłada pustego wiersza") {
    Spool one{lump("a\nb\n")};

    same(*one.counted(), std::size_t{2});
}

TRIAL("Spool: pusty dokument ma zero wierszy") {
    Spool one{lump("")};

    same(*one.counted(), std::size_t{0});
    same(one.lines(0, 5).size(), std::size_t{0});
    ought(!one.at(0).has_value());
}

TRIAL("Spool: powrót karetki znika z końca wiersza") {
    Spool one{lump("x\r\ny\r\n")};

    const std::vector<std::string> got = one.lines(0, 5);

    same(got.size(), std::size_t{2});
    same(got[0], std::string{"x"});
    same(got[1], std::string{"y"});
}

// Plik binarny nie ma ani jednego `\n`. Bez granicy zassałby się w całości do jednego napisu,
// więc rozpada się na równe kawałki po megabajcie.
TRIAL("Spool: dokument bez końców wierszy rozpada się na kawałki") {
    Spool one{lump(std::string(3u * 1024 * 1024 + 7, 'z'))};

    same(*one.counted(), std::size_t{4});

    const std::vector<std::string> got = one.lines(3, 1);

    same(got.size(), std::size_t{1});
    same(got[0].size(), std::size_t{7});
}

// --- Źródło strumieniowe -------------------------------------------------------------------------

TRIAL("Spool: strumień nie zna liczby wierszy przed dojściem do końca") {
    Taped tape{many(2000)};

    Spool one{tape.feed(), tape.whole()};

    ought(!one.counted().has_value());

    (void)one.lines(0, 5);

    ought(!one.counted().has_value());
}

// To jest ta chwila, w której w Midnight Commanderze znika `+`.
TRIAL("Spool: liczba wierszy pojawia się po dojściu do końca") {
    Taped tape{many(2000)};

    Spool one{tape.feed(), tape.whole()};

    (void)one.lines(1999, 5);

    ought(one.counted().has_value());
    same(*one.counted(), std::size_t{2000});
}

TRIAL("Spool: skok w przód i powrót dają to samo, co dokument w pamięci") {
    const std::string said = many(20000);

    Taped tape{said};

    Spool one{tape.feed(), tape.whole()};
    Spool wzor{lump(said)};

    one.screen(40);

    for (const std::size_t which : {std::size_t{0}, std::size_t{15000}, std::size_t{14999},
                                    std::size_t{1}, std::size_t{19999}}) {
        const std::vector<std::string> got = one.lines(which, 2);
        const std::vector<std::string> want = wzor.lines(which, 2);

        same(got, want);
        same(*one.at(which), *wzor.at(which));
    }
}

// Cofanie się jest jedyną trudną operacją, bo poprzedniego początku wiersza nie da się policzyć —
// trzeba go znaleźć. Przechodzimy więc cały dokument **w tył**, wiersz po wierszu.
TRIAL("Spool: krok w tył przez cały dokument nie gubi ani jednego wiersza") {
    Taped tape{many(3000, "")};

    Spool one{tape.feed(), tape.whole()};

    (void)one.lines(2999, 1);

    for (int which = 2999; which >= 0; --which) {
        const std::vector<std::string> got = one.lines(static_cast<std::size_t>(which), 1);

        same(got.size(), std::size_t{1});
        same(got[0], std::to_string(which));
    }
}

// Okno bajtów ma sens tylko wtedy, gdy przewijanie po sąsiedztwie z niego korzysta. Sto wierszy
// zwykłego tekstu mieści się w jednym oknie, więc wołanie powinno być dokładnie jedno.
TRIAL("Spool: przewijanie po sąsiedztwie nie schodzi do źródła przy każdym wierszu") {
    Taped tape{many(100)};

    Spool one{tape.feed(), tape.whole()};

    for (std::size_t which = 0; which < 100; ++which) (void)one.lines(which, 1);

    same(tape.calls(), 1);
}

// **Źródło ma prawo oddać mniej, niż poproszono** — plik czytany kawałkami, gniazdo, cokolwiek.
// Cofanie się skanuje całe okno wstecz, więc jedna krótka odpowiedź wywracała je na sam początek
// dokumentu: zamiast poprzedniego wiersza wychodził wiersz zerowy.
TRIAL("Spool: źródło oddające po sto bajtów nie psuje cofania") {
    const std::string said = many(4000);

    Bytes raw = lump(said);

    Feed feed = [raw](std::size_t at, std::size_t most) {
        Bytes made;

        const std::size_t give = most < 100 ? most : 100;

        for (std::size_t one = at; one < raw.size() && one < at + give; ++one)
            made.push_back(raw[one]);

        return made;
    };

    Spool one{feed, raw.size()};

    (void)one.lines(3999, 1);

    for (int which = 3999; which >= 3900; --which) {
        const std::vector<std::string> got = one.lines(static_cast<std::size_t>(which), 1);

        same(got.size(), std::size_t{1});
        same(got[0], "wiersz " + std::to_string(which));
    }
}

TRIAL("Spool: mówi, który wiersz zawiera dany bajt") {
    Spool one{lump("a\nbb\nccc")};

    same(one.found(0), std::size_t{0});
    same(one.found(1), std::size_t{0});
    same(one.found(2), std::size_t{1});
    same(one.found(4), std::size_t{1});
    same(one.found(5), std::size_t{2});
    same(one.found(7), std::size_t{2});
    same(one.found(1000), std::size_t{2});
}

TRIAL("Spool: bajt i wiersz zgadzają się w obie strony na całej długości") {
    const std::string said = many(5000);

    Spool one{lump(said)};

    for (std::size_t which = 0; which < 5000; which += 137)
        same(one.found(*one.at(which)), which);
}
