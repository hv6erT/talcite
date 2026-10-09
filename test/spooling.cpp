#include "proving.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "talcite/bytes.hpp"
#include "talcite/loop.hpp"
#include "talcite/promise.hpp"

#include "quilt.hpp"
#include "spool.hpp"

using namespace talcite;

namespace {


// Źródło asynchroniczne z pamięci: oddaje obietnicę, która spełnia się na puli — jak `File::read`
// z talcite::fs, tylko bez dysku. `give` ogranicza jedną odpowiedź, bo źródło **ma prawo oddać
// mniej, niż poproszono**.
Feed fed(const Bytes& raw, std::size_t give = SIZE_MAX) {
    return [raw, give](std::size_t at, std::size_t most) {
        Bytes made;

        const std::size_t much = most < give ? most : give;

        for (std::size_t one = at; one < raw.size() && one < at + much; ++one) made.push_back(raw[one]);

        return Promise<Bytes>{[made](auto resolve, auto) { resolve(made); }};
    };
}

// Źródło, które **liczy wołania**. Na nim widać, czy okno bajtów naprawdę oszczędza: przewijanie
// po sąsiedztwie nie ma prawa schodzić do niego przy każdym wierszu. Liczy przy zamówieniu, na
// wątku głównym — więc licznik można czytać bez zamka.
class Taped {
public:
    explicit Taped(std::string what) : raw_(bytes(what)) {}

    [[nodiscard]] std::size_t whole() const { return raw_.size(); }

    [[nodiscard]] int calls() const { return calls_; }

    [[nodiscard]] Feed feed() {
        return [this](std::size_t at, std::size_t most) {
            ++calls_;

            return fed(raw_)(at, most);
        };
    }

private:
    Bytes raw_;
    int calls_ = 0;
};

// Szpula na źródle asynchronicznym musi żyć w `shared_ptr` — okno wraca do niej słabym uchwytem.
std::shared_ptr<Spool> spooled(const Feed& feed, std::size_t whole) { return std::make_shared<Spool>(feed, whole); }

// Pyta, aż szpula przestanie czekać na okno; między pytaniami obraca pętlę, jak zrobiłby `Term`.
template <typename Fn>
auto settled(Loop& loop, Spool& one, Fn fn) {
    for (int tries = 0; tries < 4000; ++tries) {
        auto got = fn();

        if (!one.stalled()) return got;

        (void)loop.bell().wait(std::chrono::milliseconds{5});
        (void)loop.turn();
    }

    throw std::runtime_error{"The spool never settled."};
}

std::vector<std::string> rolled(Loop& loop, Spool& one, std::size_t which, std::size_t most) {
    return settled(loop, one, [&] { return one.lines(which, most); }).value_or(std::vector<std::string>{});
}

std::optional<std::size_t> placed(Loop& loop, Spool& one, std::size_t which) {
    return settled(loop, one, [&] { return one.at(which); });
}

std::string many(int much, const char* said = "wiersz ") {
    std::string made;

    for (int one = 0; one < much; ++one) made += said + std::to_string(one) + "\n";

    return made;
}

}

// --- Dokument w pamięci --------------------------------------------------------------------------

TRIAL("Spool: liczy wiersze przy podaniu") {
    Spool one{bytes("a\nbb\nccc")};

    ought(one.counted().has_value());
    same(*one.counted(), std::size_t{3});
    same(one.whole(), std::size_t{8});
}

TRIAL("Spool: oddaje treść bez końca wiersza") {
    Spool one{bytes("a\nbb\nccc")};

    const std::vector<std::string> got = *one.lines(0, 10);

    same(got.size(), std::size_t{3});
    same(got[0], std::string{"a"});
    same(got[1], std::string{"bb"});
    same(got[2], std::string{"ccc"});
}

TRIAL("Spool: zna bajt początku każdego wiersza") {
    Spool one{bytes("a\nbb\nccc")};

    same(*one.at(0), std::size_t{0});
    same(*one.at(1), std::size_t{2});
    same(*one.at(2), std::size_t{5});
    ought(!one.at(3).has_value());
}

// Koniec wiersza na końcu pliku **nie robi** pustego wiersza po sobie. Inaczej każdy porządnie
// zakończony plik miałby o jeden wiersz za dużo, a taki jest niemal każdy.
TRIAL("Spool: enter na końcu nie dokłada pustego wiersza") {
    Spool one{bytes("a\nb\n")};

    same(*one.counted(), std::size_t{2});
}

TRIAL("Spool: pusty dokument ma zero wierszy") {
    Spool one{bytes("")};

    same(*one.counted(), std::size_t{0});
    same(one.lines(0, 5)->size(), std::size_t{0});
    ought(!one.at(0).has_value());
}

TRIAL("Spool: powrót karetki znika z końca wiersza") {
    Spool one{bytes("x\r\ny\r\n")};

    const std::vector<std::string> got = *one.lines(0, 5);

    same(got.size(), std::size_t{2});
    same(got[0], std::string{"x"});
    same(got[1], std::string{"y"});
}

// Plik binarny nie ma ani jednego `\n`. Bez granicy zassałby się w całości do jednego napisu,
// więc rozpada się na równe kawałki po megabajcie.
TRIAL("Spool: dokument bez końców wierszy rozpada się na kawałki") {
    Spool one{bytes(std::string(3u * 1024 * 1024 + 7, 'z'))};

    same(*one.counted(), std::size_t{4});

    const std::vector<std::string> got = *one.lines(3, 1);

    same(got.size(), std::size_t{1});
    same(got[0].size(), std::size_t{7});
}

// --- Źródło strumieniowe -------------------------------------------------------------------------

TRIAL("Spool: strumień nie zna liczby wierszy przed dojściem do końca") {
    Loop loop;
    Taped tape{many(2000)};

    const auto one = spooled(tape.feed(), tape.whole());

    ought(!one->counted().has_value());

    (void)rolled(loop, *one, 0, 5);

    ought(!one->counted().has_value());
}

// Pierwsze pytanie nie ma czym odpowiedzieć: okno jest dopiero zamówione. Odpowiedź to pustka
// zamiast wektora i `stalled()`; po obrocie pętli okno jest i wiersze są. Nic tu nie czekało.
TRIAL("Spool: na pierwsze pytanie odpowiada zamówieniem, a po obrocie pętli wierszami") {
    Loop loop;
    Taped tape{many(50)};

    const auto one = spooled(tape.feed(), tape.whole());

    int woken = 0;

    one->awoken([&woken] { ++woken; });

    const std::optional<std::vector<std::string>> first = one->lines(0, 5);

    ought(!first.has_value());
    ought(one->stalled());
    same(tape.calls(), 1);
    same(woken, 0);

    // Drugie pytanie przed obrotem nie zamawia drugi raz — jedno okno w drodze naraz.
    ought(!one->lines(0, 5).has_value());
    same(tape.calls(), 1);

    const std::vector<std::string> got = rolled(loop, *one, 0, 5);

    same(got.size(), std::size_t{5});
    same(got[0], std::string{"wiersz 0"});
    ought(!one->stalled());
    same(woken, 1);
}

// To jest ta chwila, w której w Midnight Commanderze znika `+`.
TRIAL("Spool: liczba wierszy pojawia się po dojściu do końca") {
    Loop loop;
    Taped tape{many(2000)};

    const auto one = spooled(tape.feed(), tape.whole());

    (void)rolled(loop, *one, 1999, 5);

    ought(one->counted().has_value());
    same(*one->counted(), std::size_t{2000});
}

// `counting()` idzie do końca samo, okno po oknie — nikt nie pyta w międzyczasie; a gdy skończy,
// budzi uchwyt, żeby `End` mógł się dokończyć.
TRIAL("Spool: counting dochodzi do końca okno po oknie i budzi uchwyt") {
    Loop loop;
    Taped tape{many(30000)};

    const auto one = spooled(tape.feed(), tape.whole());

    int woken = 0;

    one->awoken([&woken] { ++woken; });
    one->counting();

    ought(!one->counted().has_value());

    for (int tries = 0; tries < 4000 && !one->counted().has_value(); ++tries) {
        (void)loop.bell().wait(std::chrono::milliseconds{5});
        (void)loop.turn();
    }

    same(*one->counted(), std::size_t{30000});
    ought(woken > 1);
    ought(tape.calls() > 1);

    // Po policzeniu koniec jest pod ręką: ostatni wiersz bez nowego zamówienia.
    const int calls = tape.calls();

    const std::vector<std::string> got = rolled(loop, *one, 29999, 1);

    same(got.size(), std::size_t{1});
    same(got[0], std::string{"wiersz 29999"});
    same(tape.calls(), calls);
}

TRIAL("Spool: skok w przód i powrót dają to samo, co dokument w pamięci") {
    Loop loop;

    const std::string said = many(20000);

    Taped tape{said};

    const auto one = spooled(tape.feed(), tape.whole());
    Spool wzor{bytes(said)};

    one->screen(40);

    for (const std::size_t which : {std::size_t{0}, std::size_t{15000}, std::size_t{14999},
                                    std::size_t{1}, std::size_t{19999}}) {
        const std::vector<std::string> got = rolled(loop, *one, which, 2);
        const std::vector<std::string> want = *wzor.lines(which, 2);

        same(got, want);
        same(*placed(loop, *one, which), *wzor.at(which));
    }
}

// Cofanie się jest jedyną trudną operacją, bo poprzedniego początku wiersza nie da się policzyć —
// trzeba go znaleźć. Przechodzimy więc cały dokument **w tył**, wiersz po wierszu.
TRIAL("Spool: krok w tył przez cały dokument nie gubi ani jednego wiersza") {
    Loop loop;
    Taped tape{many(3000, "")};

    const auto one = spooled(tape.feed(), tape.whole());

    (void)rolled(loop, *one, 2999, 1);

    for (int which = 2999; which >= 0; --which) {
        const std::vector<std::string> got = rolled(loop, *one, static_cast<std::size_t>(which), 1);

        same(got.size(), std::size_t{1});
        same(got[0], std::to_string(which));
    }
}

// Okno bajtów ma sens tylko wtedy, gdy przewijanie po sąsiedztwie z niego korzysta. Sto wierszy
// zwykłego tekstu mieści się w jednym oknie, więc zamówienie powinno być dokładnie jedno.
TRIAL("Spool: przewijanie po sąsiedztwie nie schodzi do źródła przy każdym wierszu") {
    Loop loop;
    Taped tape{many(100)};

    const auto one = spooled(tape.feed(), tape.whole());

    for (std::size_t which = 0; which < 100; ++which) (void)rolled(loop, *one, which, 1);

    same(tape.calls(), 1);
}

// Przy dokumencie dłuższym niż okno następne okno zamawia się **zanim** ktoś o nie zapyta: gdy
// oddajemy wiersze z końca tego, co mamy, zamówienie już biegnie. Wiersz z drugiego okna wychodzi
// wtedy bez pustej odpowiedzi — przewijanie w dół nie ma przystanków.
TRIAL("Spool: okno w drodze, zanim przewijanie do niego dojdzie") {
    Loop loop;
    Taped tape{many(20000)};

    const auto one = spooled(tape.feed(), tape.whole());

    int woken = 0;

    one->awoken([&woken] { ++woken; });

    (void)rolled(loop, *one, 0, 40);

    same(tape.calls(), 1);

    // Wiersz 3100 leży około 40 KB od początku: w pierwszym oknie, ale bliżej jego końca niż pół
    // okna — to wystarczy, żeby drugie okno poszło w drogę bez pytania.
    (void)rolled(loop, *one, 3100, 40);

    same(tape.calls(), 2);

    for (int tries = 0; tries < 4000 && woken < 2; ++tries) {
        (void)loop.bell().wait(std::chrono::milliseconds{5});
        (void)loop.turn();
    }

    same(woken, 2);

    // Wiersz 5000 jest już w drugim oknie: odpowiedź od ręki, bez zamówienia.
    const std::optional<std::vector<std::string>> got = one->lines(5000, 40);

    ought(got.has_value());
    same(got->size(), std::size_t{40});
    same(got->front(), std::string{"wiersz 5000"});
    ought(!one->stalled());
    same(tape.calls(), 2);
}

// **Źródło ma prawo oddać mniej, niż poproszono** — plik czytany kawałkami, gniazdo, cokolwiek.
// Cofanie się skanuje całe okno wstecz, więc jedna krótka odpowiedź wywracała je na sam początek
// dokumentu: zamiast poprzedniego wiersza wychodził wiersz zerowy.
TRIAL("Spool: źródło oddające po sto bajtów nie psuje cofania") {
    Loop loop;

    const std::string said = many(4000);

    const Bytes raw = bytes(said);

    const auto one = spooled(fed(raw, 100), raw.size());

    (void)rolled(loop, *one, 3999, 1);

    for (int which = 3999; which >= 3900; --which) {
        const std::vector<std::string> got = rolled(loop, *one, static_cast<std::size_t>(which), 1);

        same(got.size(), std::size_t{1});
        same(got[0], "wiersz " + std::to_string(which));
    }
}

// Źródło, które odmawia, nie zamyka programu w pętli zamówień: dokument kończy się tam, dokąd
// doszliśmy, i od tej chwili nikt już o nic nie pyta.
TRIAL("Spool: odmowa źródła kończy dokument tam, dokąd doszedł") {
    Loop loop;

    const Bytes raw = bytes(many(20000));

    int calls = 0;

    const Feed feed = [&calls, raw](std::size_t at, std::size_t most) {
        ++calls;

        if (at > 0) return Promise<Bytes>{[](auto, auto reject) { reject(std::make_exception_ptr(std::runtime_error{"gone"})); }};

        return fed(raw)(at, most);
    };

    const auto one = spooled(feed, raw.size());

    const std::vector<std::string> first = rolled(loop, *one, 0, 5);

    same(first.size(), std::size_t{5});

    const std::vector<std::string> far = rolled(loop, *one, 19000, 5);

    ought(far.empty());
    ought(one->counted().has_value());
    ought(*one->counted() < 20000);

    const int was = calls;

    (void)rolled(loop, *one, 19000, 5);
    (void)rolled(loop, *one, 5000, 5);

    same(calls, was);
}

TRIAL("Spool: mówi, który wiersz zawiera dany bajt") {
    Spool one{bytes("a\nbb\nccc")};

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

    Spool one{bytes(said)};

    for (std::size_t which = 0; which < 5000; which += 137)
        same(one.found(*one.at(which)), which);
}

// **Po zmianie kotwice przesuwają się, a nie giną.** Szpula doszła raz do dalekiego wiersza i zna po
// drodze kotwice. Zmiana na początku (wstawienie z nowymi wierszami, a potem skasowanie końca
// wiersza) przesuwa je — skok z powrotem w to samo miejsce czyta okolicę kotwicy, a nie wszystko od
// miejsca zmiany. Wynik zgadza się z liczeniem od zera.
TRIAL("Spool: po zmianie kotwice przesuwają się, a skok daleko nie czyta od nowa") {
    std::string doc;

    for (int at = 0; at < 60000; ++at) doc += "wiersz " + std::to_string(at) + "\n";

    auto quilt = std::make_shared<Quilt>(bytes(doc));
    auto read = std::make_shared<std::size_t>(0);

    const auto spool = std::make_shared<Spool>(
        [quilt, read](std::size_t at, std::size_t most) {
            Bytes got = quilt->bytes(at, most);

            *read += got.size();

            return got;
        },
        quilt->whole());

    ought(spool->at(50000).has_value());

    const auto started = [&doc](std::size_t line) {
        std::size_t at = 0;

        for (std::size_t one = 0; one < line; ++one) at = doc.find('\n', at) + 1;

        return at;
    };

    // Koniec wiersza tuż przed kotwicą (co 64 wiersze): kotwica leży w skasowanym odcinku i ginie —
    // jej wiersz zaczyna się teraz gdzie indziej.
    const std::size_t joined = started(192) - 1;
    doc.erase(joined, 1);
    quilt->erase(joined, 1);
    spool->changed(joined, 1, 0, -1);

    doc.insert(5, "ab\ncd\n");
    quilt->insert(5, "ab\ncd\n");
    spool->changed(5, 0, 6, 2);

    const std::size_t gone = doc.find('\n', 20);
    doc.erase(gone, 1);
    quilt->erase(gone, 1);
    spool->changed(gone, 1, 0, -1);

    // Miejsce, w którym szpula stała, przesunęło się razem z kotwicami: pytanie o nie nie czyta nic.
    *read = 0;

    same(spool->at(50000), std::optional<std::size_t>{started(50000)});
    same(*read, std::size_t{0});

    same(spool->at(50001), std::optional<std::size_t>{started(50001)});
    ought(*read < 200000);

    for (const std::size_t line : {0U, 1U, 2U, 3U, 4U, 100U, 190U, 191U, 192U, 25000U, 50000U})
        same(spool->at(line), std::optional<std::size_t>{started(line)});

    same(spool->found(started(40000) + 3), std::size_t{40000});
}

// **Skok daleko od miejsca, w którym szpula stoi, zaczyna od najbliższej kotwicy** — w obie strony:
// wiersz po numerze i wiersz po bajcie. Kotwic jest kilka tysięcy i szuka się ich połówkowo; gdyby
// szukanie oddało kotwicę zerową, skok w środek przeczytałby dokument od początku albo cofał się
// wiersz po wierszu od miejsca, w którym szpula stoi.
TRIAL("Spool: skok daleko zaczyna od najbliższej kotwicy") {
    std::string doc;

    for (int at = 0; at < 60000; ++at) doc += "wiersz " + std::to_string(at) + "\n";

    auto quilt = std::make_shared<Quilt>(bytes(doc));
    auto read = std::make_shared<std::size_t>(0);

    const auto spool = std::make_shared<Spool>(
        [quilt, read](std::size_t at, std::size_t most) {
            Bytes got = quilt->bytes(at, most);

            *read += got.size();

            return got;
        },
        quilt->whole());

    ought(spool->at(59000).has_value());

    const auto started = [&doc](std::size_t line) {
        std::size_t at = 0;

        for (std::size_t one = 0; one < line; ++one) at = doc.find('\n', at) + 1;

        return at;
    };

    *read = 0;

    same(spool->at(35000), std::optional<std::size_t>{started(35000)});
    ought(*read < 100000);

    ought(spool->at(59000).has_value());

    *read = 0;

    same(spool->found(started(30000) + 3), std::size_t{30000});
    ought(*read < 100000);
}

// Wiele zmian naraz (`rewritten`): wszystko za pierwszą zmianą idzie do kosza, także kotwica tuż za
// nią — jej wiersz przy skasowanym końcu wiersza zaczyna się gdzie indziej.
TRIAL("Spool: przepisanie od miejsca wyrzuca kotwice tuż za nim") {
    std::string doc;

    for (int at = 0; at < 1000; ++at) doc += "w" + std::to_string(at) + "\n";

    auto quilt = std::make_shared<Quilt>(bytes(doc));

    const auto spool = std::make_shared<Spool>([quilt](std::size_t at, std::size_t most) { return quilt->bytes(at, most); },
                                               quilt->whole());

    ought(spool->at(500).has_value());

    std::size_t joined = 0;

    for (int line = 0; line < 128; ++line) joined = doc.find('\n', joined) + 1;

    --joined;

    doc.erase(joined, 1);
    quilt->erase(joined, 1);
    spool->rewritten(joined, -1, -1);

    std::size_t at = 0;

    for (int line = 0; line < 127; ++line) at = doc.find('\n', at) + 1;

    same(spool->at(127), std::optional<std::size_t>{at});
    same(spool->found(at + 2), std::size_t{127});
    same(spool->found(joined + 1), std::size_t{127});
    same(spool->at(128), std::optional<std::size_t>{doc.find('\n', joined) + 1});
}
