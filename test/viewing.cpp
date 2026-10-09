#include "proving.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <type_traits>
#include <vector>

#include "talcite/bytes.hpp"
#include "talcite/editor.hpp"
#include "talcite/encoding.hpp"
#include "talcite/feed.hpp"
#include "talcite/loop.hpp"
#include "talcite/promise.hpp"
#include "talcite/root.hpp"
#include "talcite/viewer.hpp"

using namespace talcite;

namespace {

class Sheet : public ui::Root {};


// Wiersz kartki jako napis, z obciętymi spacjami z prawej — inaczej każde porównanie musiałoby
// dopisywać dopełnienie do szerokości okna.
std::string said(const View& one, int y) {
    std::string made;

    for (int x = 0; x < one.size().wide; ++x) {
        const char32_t letter = one.at({x, y}).letter;

        if (letter == 0) continue;

        utf8::encode(letter, made);
    }

    while (!made.empty() && made.back() == ' ') made.pop_back();

    return made;
}

std::shared_ptr<ui::Viewer> shown(Sheet& sheet, const std::string& what) {
    const auto one = std::make_shared<ui::Viewer>();

    one->hold(bytes(what));

    sheet.children().add(one);

    return one;
}

// Źródło asynchroniczne z pamięci — jak `File::read`, tylko bez dysku.
Feed fed(const Bytes& raw) {
    return [raw](std::size_t at, std::size_t most) {
        Bytes made;

        for (std::size_t one = at; one < raw.size() && one < at + most; ++one) made.push_back(raw[one]);

        return Promise<Bytes>{[made](auto resolve, auto) { resolve(made); }};
    };
}

// Obraca pętlę i maluje, aż warunek stanie się prawdą — tak, jak robi to `Term` między klatkami.
template <typename Fn>
bool turned(Loop& loop, Sheet& sheet, ui::Size size, Fn until) {
    for (int tries = 0; tries < 4000; ++tries) {
        (void)sheet.render({{0, 0}, size});

        if (until()) return true;

        (void)loop.bell().wait(std::chrono::milliseconds{5});
        (void)loop.turn();
    }

    return false;
}

std::string many(int much) {
    std::string made;

    for (int one = 0; one < much; ++one) made += "wiersz " + std::to_string(one) + "\n";

    return made;
}

}

// --- Co widżet wie przed układem -----------------------------------------------------------------

TRIAL("Viewer: pusty nic nie niesie") {
    const ui::Viewer one;

    ought(!one.held());
    ought(!one.first().has_value());
    ought(!one.lines().has_value());
    ought(!one.at().has_value());
    ought(!one.whole().has_value());
}

TRIAL("Viewer: dokument w pamięci daje komplet wartości od razu") {
    Sheet sheet;

    const auto one = shown(sheet, "a\nbb\nccc");

    ought(one->held());
    same(*one->first(), std::size_t{0});
    same(*one->lines(), std::size_t{3});
    same(*one->at(), std::size_t{0});
    same(*one->whole(), std::size_t{8});
}

// Źródło wierszowe bajtami nie myśli, więc `at` i `whole` zostają puste. To nie jest brak, tylko
// prawda o tym, co podano.
TRIAL("Viewer: źródło wierszowe nie zna bajtów") {
    Sheet sheet;

    const auto one = std::make_shared<ui::Viewer>();

    one->hold([](std::size_t which, std::size_t most) {
        std::vector<std::string> made;

        for (std::size_t at = which; at < 10 && made.size() < most; ++at)
            made.push_back("w" + std::to_string(at));

        return std::optional{made};
    },
              10);

    sheet.children().add(one);

    ought(one->held());
    same(*one->lines(), std::size_t{10});
    same(*one->first(), std::size_t{0});
    ought(!one->at().has_value());
    ought(!one->whole().has_value());
}

// **Dokument trzyma strona i nowy zastępuje poprzedni w całości.** Dawniej podgląd trzymał
// własną szpulę i po przejściu z bajtów na zwój odpowiadał liczbą wierszy starego dokumentu.
TRIAL("Viewer: zwój po bajtach zastępuje dokument w całości") {
    Sheet sheet;

    const auto one = shown(sheet, "a\nbb\nccc");

    same(*one->lines(), std::size_t{3});

    one->hold([](std::size_t which, std::size_t most) {
        std::vector<std::string> made;

        for (std::size_t at = which; at < 10 && made.size() < most; ++at) made.push_back("w" + std::to_string(at));

        return std::optional{made};
    },
              10);

    same(*one->lines(), std::size_t{10});
    ought(!one->at().has_value());
    ought(!one->whole().has_value());

    one->hold(bytes("x\ny"));

    same(*one->lines(), std::size_t{2});
    same(*one->at(), std::size_t{0});
    same(*one->whole(), std::size_t{3});
}

// Skok po bajcie stoi w stronie, więc zwój — który bajtów nie zna — go ignoruje, a szpula wykonuje.
TRIAL("Viewer: skok po bajcie działa przy bajtach, przy zwoju nic") {
    Sheet sheet;

    const auto one = shown(sheet, "a\nbb\nccc");

    one->at(std::size_t{5});

    same(*one->first(), std::size_t{2});
    same(*one->at(), std::size_t{5});

    one->hold([](std::size_t which, std::size_t most) {
        std::vector<std::string> made;

        for (std::size_t at = which; at < 10 && made.size() < most; ++at) made.push_back("w" + std::to_string(at));

        return std::optional{made};
    },
              10);

    one->at(std::size_t{5});

    same(*one->first(), std::size_t{0});
}

// Czego już nie da się napisać: gołej strony, zwoju w edytorze — który umie edytować tylko to,
// co sam trzyma w kołdrze — i dokumentu podanego stronie z zewnątrz.
namespace {

template <typename One>
concept rolled = requires(One& one, Roll given) { one.hold(given, std::size_t{0}); };

}

static_assert(!std::is_default_constructible_v<ui::Page>);
static_assert(!rolled<ui::Editor>);
static_assert(!rolled<ui::Page>);
static_assert(rolled<ui::Viewer>);

// --- Rysowanie -----------------------------------------------------------------------------------

TRIAL("Viewer: wiersze lądują jeden pod drugim") {
    Sheet sheet;

    const auto one = shown(sheet, "pierwszy\ndrugi\ntrzeci");

    const View made = sheet.render({{0, 0}, {20, 4}});

    same(said(made, 0), std::string{"pierwszy"});
    same(said(made, 1), std::string{"drugi"});
    same(said(made, 2), std::string{"trzeci"});
    same(said(made, 3), std::string{});
}

TRIAL("Viewer: tabulator rozwija się do ośmiu kolumn") {
    Sheet sheet;

    const auto one = shown(sheet, "ab\tc");

    const View made = sheet.render({{0, 0}, {20, 1}});

    same(said(made, 0), std::string{"ab      c"});
}

// Zawijanie jest domyślne, więc długi wiersz zajmuje kilka wierszy ekranu.
TRIAL("Viewer: zawijanie dzieli długi wiersz po kolumnach") {
    Sheet sheet;

    const auto one = shown(sheet, "aaaabbbbcccc");

    ought(one->wrapped());

    const View made = sheet.render({{0, 0}, {4, 4}});

    same(said(made, 0), std::string{"aaaa"});
    same(said(made, 1), std::string{"bbbb"});
    same(said(made, 2), std::string{"cccc"});
}

TRIAL("Viewer: bez zawijania wiersz się urywa") {
    Sheet sheet;

    const auto one = shown(sheet, "aaaabbbbcccc\ndrugi");

    one->wrapped(false);

    const View made = sheet.render({{0, 0}, {4, 4}});

    same(said(made, 0), std::string{"aaaa"});
    same(said(made, 1), std::string{"drug"});
}

// Znak chiński zajmuje dwie kratki, więc cięcie po bajtach albo po znakach wypuściłoby go poza
// kartkę. Przy szerokości pięciu mieszczą się dwa i zostaje jedna kolumna.
TRIAL("Viewer: cięcie liczy kolumny, nie znaki") {
    Sheet sheet;

    const auto one = shown(sheet, "\xe4\xbd\xa0\xe4\xbd\xa0\xe4\xbd\xa0");

    const View made = sheet.render({{0, 0}, {5, 3}});

    same(said(made, 0), std::string{"\xe4\xbd\xa0\xe4\xbd\xa0"});
    same(said(made, 1), std::string{"\xe4\xbd\xa0"});
}

// --- Przewijanie ---------------------------------------------------------------------------------

TRIAL("Viewer: krok w dół i w górę wraca w to samo miejsce") {
    Sheet sheet;

    const auto one = shown(sheet, many(100));

    (void)sheet.render({{0, 0}, {40, 10}});

    one->slide(5);
    same(*one->first(), std::size_t{5});

    one->slide(-5);
    same(*one->first(), std::size_t{0});
}

TRIAL("Viewer: na początku nie da się cofnąć") {
    Sheet sheet;

    const auto one = shown(sheet, many(10));

    (void)sheet.render({{0, 0}, {40, 4}});

    one->slide(-5);

    same(*one->first(), std::size_t{0});
}

// Krok w dół zatrzymuje się, gdy ostatni wiersz stoi **na dole** ekranu — nie na górze z pustką
// pod nim, jak przedtem, gdy pomoc dawała się zsunąć do jednego wiersza. Dokument krótszy niż
// ekran nie przewija się wcale, a skok do wiersza za końcem ląduje na ostatniej stronie.
TRIAL("Viewer: za ostatnią stronę nie wychodzimy") {
    Sheet sheet;

    const auto one = shown(sheet, many(10));

    (void)sheet.render({{0, 0}, {40, 4}});

    one->slide(1000);

    same(*one->first(), std::size_t{6});

    one->first(1000);

    same(*one->first(), std::size_t{6});

    Sheet other;

    const auto brief = shown(other, many(3));

    (void)other.render({{0, 0}, {40, 4}});

    brief->slide(1000);

    same(*brief->first(), std::size_t{0});
}

// Przy zawijaniu jeden wiersz dokumentu to kilka kroków ekranu, więc krok nie przesuwa numeru
// wiersza za każdym razem.
TRIAL("Viewer: przy zawijaniu krok idzie po kawałkach") {
    Sheet sheet;

    const auto one = shown(sheet, "aaaabbbbcccc\ndrugi");

    (void)sheet.render({{0, 0}, {4, 2}});

    one->slide(1);
    same(*one->first(), std::size_t{0});

    one->slide(1);
    same(*one->first(), std::size_t{0});

    one->slide(1);
    same(*one->first(), std::size_t{1});

    // Pięć kawałków na dwóch wierszach ekranu: dalej już nie ma dokąd.
    one->slide(1);
    same(*one->first(), std::size_t{1});
}

TRIAL("Viewer: strona przesuwa o wysokość okna") {
    Sheet sheet;

    const auto one = shown(sheet, many(100));

    (void)sheet.render({{0, 0}, {40, 10}});

    one->focus();

    ui::KeyEvent down{ui::Code::pagedown};

    ought(sheet.dispatch(down));

    same(*one->first(), std::size_t{10});
}

// `slide` mówi, czy obraz się ruszył, i zawiadamia tylko wtedy. Na końcu dokumentu i przy zerze
// nie ma czego zgłaszać.
TRIAL("Viewer: slide oddaje, czy się ruszyło, i tylko wtedy zawiadamia") {
    Sheet sheet;

    const auto one = shown(sheet, many(100));

    (void)sheet.render({{0, 0}, {40, 10}});

    int heard = 0;

    (void)sheet.on<ui::ScrollEvent>([&heard](ui::ScrollEvent&) {
        ++heard;

        return false;
    });

    ought(one->slide(3));
    same(heard, 1);

    ought(!one->slide(0));
    same(heard, 1);

    ought(one->slide(-10));
    same(*one->first(), std::size_t{0});
    same(heard, 2);

    ought(!one->slide(-1));
    same(heard, 2);

    one->end();
    heard = 0;

    ought(!one->slide(1));
    same(heard, 0);
}

TRIAL("Viewer: koniec pokazuje ostatnią stronę") {
    Sheet sheet;

    const auto one = shown(sheet, many(100));

    (void)sheet.render({{0, 0}, {40, 10}});

    one->end();

    same(*one->first(), std::size_t{90});

    const View made = sheet.render({{0, 0}, {40, 10}});

    same(said(made, 9), std::string{"wiersz 99"});
}

TRIAL("Viewer: skok do wiersza przycina się do dokumentu") {
    Sheet sheet;

    const auto one = shown(sheet, many(10));

    one->first(1000);

    same(*one->first(), std::size_t{9});
}

TRIAL("Viewer: skok po bajcie ląduje we właściwym wierszu") {
    Sheet sheet;

    const auto one = shown(sheet, "a\nbb\nccc");

    one->at(5);

    same(*one->first(), std::size_t{2});
    same(*one->at(), std::size_t{5});
}

// Ten sam bajt znaczy co innego w obu trybach, więc przełączenie musi zostać w tym samym wierszu
// dokumentu, a nie przeskoczyć w losowe miejsce.
TRIAL("Viewer: przełączenie zawijania nie rusza wiersza") {
    Sheet sheet;

    const auto one = shown(sheet, "aaaabbbbcccc\ndrugi\ntrzeci");

    (void)sheet.render({{0, 0}, {4, 4}});

    one->first(1);
    one->wrapped(false);

    same(*one->first(), std::size_t{1});

    const View made = sheet.render({{0, 0}, {4, 4}});

    same(said(made, 0), std::string{"drug"});
}

// **Przewijanie liczy kawałki, rysowanie je buduje — jednym chodzikiem.** Losowe dokumenty
// z tabulatorami, znakami podwójnymi, łączącymi i wierszami równymi wielokrotności szerokości:
// po każdym kroku ekran krótki musi pokazywać ten sam odcinek, co wysoki od góry. Gdyby liczenie
// rozjechało się z cięciem choćby o jeden kawałek, krok lądowałby o wiersz obok, a koniec
// dokumentu nie stałby na dole.
TRIAL("Viewer: krok po kawałkach zgadza się z tym, co narysowane") {
    std::mt19937 random{7};

    const std::vector<std::string> signs{"a", "b", " ", "\t", "\xe4\xbd\xa0", "e\xcc\x81", "\xcc\x81"};

    const auto picked = [&random](int much) { return static_cast<int>(random() % static_cast<unsigned>(much)); };

    for (int round = 0; round < 60; ++round) {
        std::string text;

        const int count = 1 + picked(12);

        for (int line = 0; line < count; ++line) {
            const int much = picked(25);

            for (int one = 0; one < much; ++one) text += signs[static_cast<std::size_t>(picked(7))];

            text += '\n';
        }

        text += "Z";

        const int wide = 3 + picked(8);
        const int high = 2 + picked(4);

        Sheet tall;

        const auto whole = shown(tall, text);

        whole->scrollbar(false);

        const View all = tall.render({{0, 0}, {wide, 1000}});

        int total = 0;

        while (said(all, total) != "Z") ++total;

        ++total;

        const int last = std::max(total - high, 0);

        Sheet sheet;

        const auto one = shown(sheet, text);

        one->scrollbar(false);

        const auto agreed = [&](int from) {
            const View made = sheet.render({{0, 0}, {wide, high}});

            for (int y = 0; y < high; ++y) same(said(made, y), said(all, from + y));
        };

        agreed(0);

        for (int step = 1; step <= total; ++step) {
            one->slide(1);

            agreed(std::min(step, last));
        }

        for (int step = last - 1; step >= 0; --step) {
            one->slide(-1);

            agreed(step);
        }

        one->end();

        agreed(last);

        one->first(0);
        one->slide(high);

        agreed(std::min(high, last));
    }
}

// **Ruch ogląda każdy wiersz dokumentu raz.** Dawniej krok w obrębie zawiniętego wiersza ciął go
// od nowa za każdym wierszem ekranu, a przycięcie do końca przechodziło ekran od nowa po każdym
// cofnięciu — strona w dół przy końcu pobierała te same wiersze po kilkadziesiąt razy.
TRIAL("Viewer: ruch pyta o każdy wiersz najwyżej raz") {
    std::map<std::size_t, int> asked;

    Sheet sheet;

    const auto one = std::make_shared<ui::Viewer>();

    one->hold([&asked](std::size_t which, std::size_t most) {
        std::vector<std::string> made;

        for (std::size_t at = which; at < 40 && made.size() < most; ++at) {
            ++asked[at];

            made.push_back(std::string(at % 3 == 0 ? 25 : 3, 'x'));
        }

        return std::optional{made};
    },
              40);

    sheet.children().add(one);

    one->scrollbar(false);

    (void)sheet.render({{0, 0}, {10, 6}});

    const auto once = [&asked, &one](int lines) {
        asked.clear();

        one->slide(lines);

        ought(!asked.empty());

        for (const auto& [line, much] : asked) same(much, 1);
    };

    for (int step = 0; step < 16; ++step) once(4);

    same(*one->first(), std::size_t{36});

    for (int step = 0; step < 16; ++step) once(-5);

    same(*one->first(), std::size_t{0});

    asked.clear();

    one->first(39);

    for (const auto& [line, much] : asked) same(much, 1);
}

// Wiersz, który czeka na okno, zatrzymuje przycięcie do końca w obie strony: przejście ekranu
// natrafiające na niego nie wie, gdzie koniec, więc niczego nie przycina, a cofanie się, które na
// niego wchodzi, staje na nim.
TRIAL("Viewer: przycięcie do końca staje na wierszu, który czeka") {
    Sheet sheet;

    const auto one = std::make_shared<ui::Viewer>();

    one->hold([](std::size_t which, std::size_t most) -> std::optional<std::vector<std::string>> {
        if (which == 5) return {};

        std::vector<std::string> made;

        for (std::size_t at = which; at < 8 && at != 5 && made.size() < most; ++at) made.push_back("w" + std::to_string(at));

        return made;
    },
              8);

    sheet.children().add(one);

    (void)sheet.render({{0, 0}, {10, 5}});

    one->first(7);

    same(*one->first(), std::size_t{5});

    one->first(3);

    same(*one->first(), std::size_t{3});

    // Cofnięcie na wiersz, który czeka, staje na jego początku — kawałków nie zna, więc krok
    // w przód idzie od razu do następnego wiersza. Niski ekran, żeby przycięcie nie ruszało.
    (void)sheet.render({{0, 0}, {10, 2}});

    one->first(6);

    same(*one->first(), std::size_t{6});

    one->slide(-1);

    same(*one->first(), std::size_t{5});

    one->slide(1);

    same(*one->first(), std::size_t{6});
}

// Tabulator za cięciem, sam na końcu wiersza, to wiersz ekranu ze spacjami — nie pustka, której
// można nie liczyć. Przy szerokości dziesięciu rozwija się do ósmej kolumny i zostaje ogonem.
TRIAL("Viewer: tabulator za cięciem zajmuje własny wiersz ekranu") {
    Sheet sheet;

    const auto one = shown(sheet, "aaaaaaaaaa\t\nZ");

    const View made = sheet.render({{0, 0}, {10, 3}});

    same(said(made, 0), std::string{"aaaaaaaaaa"});
    same(said(made, 1), std::string{});
    same(said(made, 2), std::string{"Z"});

    one->end();

    same(said(sheet.render({{0, 0}, {10, 3}}), 2), std::string{"Z"});
    same(*one->first(), std::size_t{0});
}

// `End` staje na **ostatnim kawałku** ostatniego wiersza: zawinięty ogon dokumentu stoi na dole
// ekranu, a nie jego początek na górze.
TRIAL("Viewer: koniec przy zawiniętym ostatnim wierszu pokazuje jego ogon") {
    Sheet sheet;

    const auto one = shown(sheet, "a\nabcdefghijkl");

    (void)sheet.render({{0, 0}, {4, 2}});

    one->end();

    const View made = sheet.render({{0, 0}, {4, 2}});

    same(said(made, 0), std::string{"efgh"});
    same(said(made, 1), std::string{"ijkl"});
}

// Zanim strona dostanie przydział, szerokości nie ma i nie ma czego ciąć: krok idzie po wierszach
// dokumentu, jak bez zawijania.
TRIAL("Viewer: przed układem krok idzie po wierszach dokumentu") {
    Sheet sheet;

    const auto one = shown(sheet, "aaaaaaaa\nb\nc");

    one->slide(1);

    same(*one->first(), std::size_t{1});
}

// --- Pasek przewijania ---------------------------------------------------------------------------

// Pasek zabiera jedną kolumnę z prawej, więc treść dostaje o tyle mniej — i to widać po zawijaniu,
// bo długi wiersz łamie się o kratkę wcześniej.
TRIAL("Viewer: pasek zabiera kolumnę treści") {
    Sheet sheet;

    const auto one = shown(sheet, "aaaabbbb");

    one->scrollbar(true);

    ought(one->scrollbar());

    const View made = sheet.render({{0, 0}, {5, 4}});

    // Ostatnia kolumna należy do paska, więc porównujemy sam początek wiersza.
    same(said(made, 0).substr(0, 4), std::string{"aaaa"});
    same(said(made, 1).substr(0, 4), std::string{"bbbb"});
}

// Bez paska ta sama treść mieści się w pięciu kolumnach.
TRIAL("Viewer: bez paska treść dostaje całą szerokość") {
    Sheet sheet;

    const auto one = shown(sheet, "aaaabbbb");

    ought(!one->scrollbar());

    const View made = sheet.render({{0, 0}, {5, 4}});

    same(said(made, 0), std::string{"aaaab"});
    same(said(made, 1), std::string{"bbb"});
}

// Pasek „gdy trzeba": strona, która mieści się cała, nie ma paska i zawija się na całą szerokość;
// strona dłuższa niż okno — także przez zawinięcie albo o jeden wiersz — ma go.
TRIAL("Viewer: pasek gdy trzeba tylko przy stronie dłuższej niż okno") {
    const auto barred = [](const std::string& text, ui::Size size) {
        Sheet sheet;

        const auto one = std::make_shared<ui::Viewer>(ui::ScrollShape{true, true});

        one->hold(bytes(text));
        sheet.children().add(one);

        const View made = sheet.render({{0, 0}, size});

        return std::pair{made.at({size.wide - 1, 0}).letter == U'▲', said(made, 0)};
    };

    const auto [fits, first] = barred("aaaabbbb", {5, 4});

    ought(!fits);
    same(first, std::string{"aaaab"});

    ought(!barred("a\nb\nc\nd", {5, 4}).first);
    ought(barred("a\nb\nc\nd\ne", {5, 4}).first);
    ought(barred("aaaabbbbccccdddde", {5, 4}).first);

    // Przewinięta do końca strona dalej ma pasek — reszta się mieści, ale początek jest nad oknem.
    {
        Sheet sheet;

        const auto one = std::make_shared<ui::Viewer>(ui::ScrollShape{true, true});

        one->hold(bytes("a\nb\nc\nd\ne\nf"));
        sheet.children().add(one);

        (void)sheet.render({{0, 0}, {5, 4}});

        one->focus();

        ui::KeyEvent end{ui::Code::end};

        ought(sheet.dispatch(end));

        const View made = sheet.render({{0, 0}, {5, 4}});

        same(made.at({4, 0}).letter, U'▲');
    }

    // Źródło, które dalszych wierszy jeszcze nie ma, też daje pasek: strona nie wie, że to koniec,
    // choć cztery wiersze, które już przyszły, wypełniają okno co do jednego.
    {
        Sheet sheet;

        const auto one = std::make_shared<ui::Viewer>(ui::ScrollShape{true, true});

        one->hold(
            [](std::size_t which, std::size_t most) -> std::optional<std::vector<std::string>> {
                if (which >= 4) return std::nullopt;

                std::vector<std::string> made;

                for (std::size_t at = which; at < 4 && made.size() < most; ++at) made.push_back("w" + std::to_string(at));

                return made;
            },
            10);
        sheet.children().add(one);

        same(sheet.render({{0, 0}, {5, 4}}).at({4, 0}).letter, U'▲');
    }
}

// Strzałki paska stoją w ostatniej kolumnie, u góry i u dołu.
TRIAL("Viewer: pasek stoi przy prawej krawędzi") {
    Sheet sheet;

    const auto one = shown(sheet, many(100));

    one->scrollbar(true);

    const View made = sheet.render({{0, 0}, {20, 5}});

    ought(made.at({19, 0}).letter != U' ');
    ought(made.at({19, 4}).letter != U' ');
}

// Póki liczba wierszy jest nieznana, pasek chodzi po bajtach, a jego okno to rozpiętość tego, co
// stoi na ekranie: od początku pierwszego widocznego wiersza do początku wiersza **za** ostatnim.
// Dziesięć wierszy po dziesięć tysięcy bajtów z dwudziestu to połowa dokumentu — i połowa toru.
TRIAL("Viewer: pasek po bajtach mierzy okno tym, co widać") {
    Loop loop;
    Sheet sheet;

    std::string text;

    for (int one = 0; one < 20; ++one) text += std::string(10000, static_cast<char>('a' + one)) + "\n";

    const Bytes raw = bytes(text);
    const auto one = std::make_shared<ui::Viewer>();

    one->hold(fed(raw), raw.size());
    one->wrapped(false);
    one->scrollbar(true);
    sheet.children().add(one);

    ought(turned(loop, sheet, {20, 10}, [&] { return said(sheet.render({{0, 0}, {20, 10}}), 9).starts_with("j"); }));
    ought(!one->lines().has_value());

    const View made = sheet.render({{0, 0}, {20, 10}});
    const char32_t block = sheet.skin().glyph("talcite.scroll.thumb");

    int thumb = 0;

    for (int y = 0; y < 10; ++y)
        if (made.at({19, y}).letter == block) ++thumb;

    ought(thumb >= 3);
}

// Zamiary czekające na okno należą do dokumentu, nie do strony: nowy dokument ich nie dziedziczy.
// Bez tego skok po bajcie wydany na starym pliku wykonałby się na nowym, gdy tylko doszłoby jego
// pierwsze okno — tu na bajcie daleko za końcem nowego dokumentu, czyli na jego ostatnich wierszach.
TRIAL("Viewer: nowy dokument nie dziedziczy zamiarów starego") {
    Loop loop;
    Sheet sheet;

    const std::string text = many(30000);
    const Bytes raw = bytes(text);
    const auto one = std::make_shared<ui::Viewer>();

    one->hold(fed(raw), raw.size());
    sheet.children().add(one);

    (void)sheet.render({{0, 0}, {40, 10}});

    one->end();
    one->at(text.find("wiersz 20000\n"));

    ought(!one->lines().has_value());

    const Bytes other = bytes(many(30));

    one->hold(fed(other), other.size());

    ought(turned(loop, sheet, {40, 10}, [&] { return said(sheet.render({{0, 0}, {40, 10}}), 0) == "wiersz 0"; }));

    for (int turns = 0; turns < 50; ++turns) {
        (void)loop.bell().wait(std::chrono::milliseconds{2});
        (void)loop.turn();
        (void)sheet.render({{0, 0}, {40, 10}});
    }

    same(*one->first(), std::size_t{0});
}

// --- Zaznaczenia ---------------------------------------------------------------------------------

// Znacznik to zakres bajtów **w wierszu** i rola ze skórki. Wbudowana rola `talcite.viewer.mark`
// odwraca, więc widać ją bez żadnych barw.
TRIAL("Viewer: znacznik przemalowuje dokładnie swój zakres") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdefgh");

    one->marks().add(ui::Mark{0, 2, 5, "talcite.viewer.mark"});

    const View made = sheet.render({{0, 0}, {20, 2}});

    ought(!made.at({1, 0}).style.reverse);
    ought(made.at({2, 0}).style.reverse);
    ought(made.at({4, 0}).style.reverse);
    ought(!made.at({5, 0}).style.reverse);

    // Litery zostają na miejscu — zmienia się sam styl.
    same(said(made, 0), std::string{"abcdefgh"});
}

// Znak dwukolumnowy zajmuje dwie kratki, a w wierszu jest jednym trójbajtowym znakiem. Zakres
// obejmujący go musi przemalować obie kolumny.
TRIAL("Viewer: znacznik na znaku podwójnym bierze obie kolumny") {
    Sheet sheet;

    const auto one = shown(sheet, "a\xe4\xbd\xa0" "b");

    one->marks().add(ui::Mark{0, 1, 4, "talcite.viewer.mark"});

    const View made = sheet.render({{0, 0}, {20, 2}});

    ought(!made.at({0, 0}).style.reverse);
    ought(made.at({1, 0}).style.reverse);
    ought(made.at({2, 0}).style.reverse);
    ought(!made.at({3, 0}).style.reverse);
}

// Tabulator jest jednym bajtem, ale zajmuje kilka kolumn — wszystkie dzielą jego los.
TRIAL("Viewer: znacznik na tabulatorze bierze całe rozwinięcie") {
    Sheet sheet;

    const auto one = shown(sheet, "ab\tc");

    one->marks().add(ui::Mark{0, 2, 3, "talcite.viewer.mark"});

    const View made = sheet.render({{0, 0}, {20, 2}});

    ought(!made.at({1, 0}).style.reverse);
    ought(made.at({2, 0}).style.reverse);
    ought(made.at({7, 0}).style.reverse);
    ought(!made.at({8, 0}).style.reverse);
}

// Zbiór musi być uporządkowany, bo inaczej `next` nie wie, który jest następny.
TRIAL("Viewer: znaczniki układają się po wierszu i po początku") {
    Sheet sheet;

    const auto one = shown(sheet, many(10));

    one->marks().add(ui::Mark{5, 0, 1, "a"});
    one->marks().add(ui::Mark{1, 4, 5, "b"});
    one->marks().add(ui::Mark{1, 0, 1, "c"});

    same(one->marks().many(), std::size_t{3});
    same(one->marks()[0].role, std::string{"c"});
    same(one->marks()[1].role, std::string{"b"});
    same(one->marks()[2].role, std::string{"a"});
}

TRIAL("Viewer: podmiana całości sortuje i czyści bieżący") {
    Sheet sheet;

    const auto one = shown(sheet, many(10));

    one->marks().all({ui::Mark{7, 0, 1, "a"}, ui::Mark{2, 0, 1, "b"}});

    same(one->marks().many(), std::size_t{2});
    same(one->marks()[0].line, std::size_t{2});
    ought(!one->which().has_value());

    one->marks().clear();

    same(one->marks().many(), std::size_t{0});
}

// --- Przewijanie po zaznaczonych -----------------------------------------------------------------

TRIAL("Viewer: bez znaczników nie ma dokąd skakać") {
    Sheet sheet;

    const auto one = shown(sheet, many(10));

    ought(!one->step(1));
    ought(!one->step(-1));
    ought(!one->which().has_value());
}

TRIAL("Viewer: skok bierze pierwszy znacznik niebędący przed ekranem") {
    Sheet sheet;

    const auto one = shown(sheet, many(100));

    (void)sheet.render({{0, 0}, {40, 10}});

    one->marks().all({ui::Mark{3, 0, 1, "a"}, ui::Mark{50, 0, 1, "b"}});

    ought(one->step(1));
    same(*one->which(), std::size_t{0});

    // Wiersz 3 był widoczny, więc obraz się nie ruszył.
    same(*one->first(), std::size_t{0});
}

TRIAL("Viewer: skok do znacznika spoza ekranu przewija na dół") {
    Sheet sheet;

    const auto one = shown(sheet, many(100));

    (void)sheet.render({{0, 0}, {40, 10}});

    one->marks().all({ui::Mark{50, 0, 1, "a"}});

    ought(one->step(1));
    same(*one->which(), std::size_t{0});
    same(*one->first(), std::size_t{41});
}

TRIAL("Viewer: za ostatnim znacznikiem nie ma następnego") {
    Sheet sheet;

    const auto one = shown(sheet, many(100));

    (void)sheet.render({{0, 0}, {40, 10}});

    one->marks().all({ui::Mark{2, 0, 1, "a"}, ui::Mark{4, 0, 1, "b"}});

    ought(one->step(1));
    ought(one->step(1));
    ought(!one->step(1));

    same(*one->which(), std::size_t{1});
}

TRIAL("Viewer: wstecz wraca po tych samych znacznikach") {
    Sheet sheet;

    const auto one = shown(sheet, many(100));

    (void)sheet.render({{0, 0}, {40, 10}});

    one->marks().all({ui::Mark{2, 0, 1, "a"}, ui::Mark{4, 0, 1, "b"}});

    ought(one->step(1));
    ought(one->step(1));
    ought(one->step(-1));

    same(*one->which(), std::size_t{0});

    ought(!one->step(-1));
}

// Skok przewijający ma zawiadomić **raz**, choćby ruch składał się z dwóch części.
TRIAL("Viewer: skok zawiadamia jednym zdarzeniem") {
    Sheet sheet;

    const auto one = shown(sheet, many(100));

    (void)sheet.render({{0, 0}, {40, 10}});

    one->marks().all({ui::Mark{50, 0, 1, "a"}});

    int heard = 0;

    (void)sheet.on<ui::ScrollEvent>([&heard](ui::ScrollEvent&) {
        ++heard;

        return false;
    });

    ought(one->step(1));

    same(heard, 1);
}

// --- Widoczny zakres -----------------------------------------------------------------------------

// `shown()` mówi, ile **wierszy dokumentu** widać — i to nie jest wysokość okna, gdy zawijanie
// rozkłada jeden wiersz na kilka ekranowych.
TRIAL("Viewer: widoczny zakres liczy wiersze dokumentu, nie ekranu") {
    Sheet sheet;

    const auto one = shown(sheet, many(100));

    (void)sheet.render({{0, 0}, {40, 10}});

    same(one->shown(), std::size_t{10});

    Sheet inna;

    const auto two = shown(inna, "aaaabbbbcccc\ndrugi\ntrzeci");

    (void)inna.render({{0, 0}, {4, 4}});

    same(two->shown(), std::size_t{2});
}

// --- Klawisze i zdarzenia ------------------------------------------------------------------------

TRIAL("Viewer: bierze ognisko i strzałki, a F-klawisze przepuszcza") {
    Sheet sheet;

    const auto one = shown(sheet, many(100));

    (void)sheet.render({{0, 0}, {40, 10}});

    ought(one->focusable());

    one->focus();

    ui::KeyEvent down{ui::Code::down};

    ought(sheet.dispatch(down));
    same(*one->first(), std::size_t{1});

    ui::KeyEvent f2{ui::Code::f2};

    ought(!sheet.dispatch(f2));
}

// Zdarzenie idzie w górę, więc nasłuch na przodku łapie ruch podglądu. Tak właśnie górny pasek
// dowiaduje się, że ma się przerysować.
TRIAL("Viewer: przesunięcie zawiadamia przodka") {
    Sheet sheet;

    const auto one = shown(sheet, many(100));

    (void)sheet.render({{0, 0}, {40, 10}});

    int heard = 0;

    (void)sheet.on<ui::ScrollEvent>([&heard](ui::ScrollEvent&) {
        ++heard;

        return false;
    });

    one->slide(1);

    same(heard, 1);

    one->slide(1000);

    same(heard, 2);
}

// Stanie w miejscu nie jest ruchem: na końcu dokumentu kółko nie ma czego zgłaszać.
TRIAL("Viewer: brak ruchu nie zawiadamia nikogo") {
    Sheet sheet;

    const auto one = shown(sheet, many(3));

    (void)sheet.render({{0, 0}, {40, 10}});

    int heard = 0;

    (void)sheet.on<ui::ScrollEvent>([&heard](ui::ScrollEvent&) {
        ++heard;

        return false;
    });

    one->slide(-1);

    same(heard, 0);
}

// **Kursor wędrówki schodzi leniwie.** Zbiór znaczników nie ma wskazania na stronę, więc nie ma
// jak jej powiedzieć, że się zmienił — po zmianie „następny znacznik" znaczy co innego, więc
// kursor przestaje mieć sens. Sprawdza to strona przy najbliższym pytaniu, tą samą drogą, którą
// historia przycina swoją listę.
TRIAL("Viewer: dopisanie znacznika kasuje kursor wędrówki") {
    Sheet sheet;

    const auto one = shown(sheet, many(40));

    one->marks().add(ui::Mark{2, 0, 3, "moja"});
    one->marks().add(ui::Mark{9, 0, 3, "moja"});

    (void)sheet.render({{0, 0}, {30, 6}});

    ought(one->step(1));
    ought(one->which().has_value());

    one->marks().add(ui::Mark{20, 0, 3, "moja"});

    ought(!one->which().has_value());
}

// Zbiór trzyma znaczniki **uporządkowane po miejscu w dokumencie**, niezależnie od tego, w jakiej
// kolejności przyszły — inaczej „następny" nie miałby jak powiedzieć, który jest następny.
TRIAL("Viewer: znaczniki układają się po miejscu, nie po kolejności dodania") {
    Sheet sheet;

    const auto one = shown(sheet, many(40));

    one->marks().add(ui::Mark{9, 0, 3, "moja"});
    one->marks().add(ui::Mark{2, 0, 3, "moja"});
    one->marks().add(ui::Mark{2, 5, 7, "moja"});

    same(one->marks().many(), std::size_t{3});
    same(one->marks()[0].line, std::size_t{2});
    same(one->marks()[0].from, std::size_t{0});
    same(one->marks()[1].from, std::size_t{5});
    same(one->marks()[2].line, std::size_t{9});

    std::size_t counted = 0;

    for (const ui::Mark& said : one->marks()) {
        (void)said;

        ++counted;
    }

    same(counted, std::size_t{3});
}

// **`MarkedNode` jest bazą, nie tylko polem strony.** Cudzy widżet dokumentowy dostaje i zbiór
// znaczników, i kursor wędrówki razem z jego leniwym kasowaniem — nie pisząc ani linijki.
TRIAL("MarkedNode: cudzy widżet dostaje znaczniki i kursor bez pisania czegokolwiek") {
    class Mine : public ui::MarkedNode<ui::Node> {
    public:
        // Jedyne, co widżet dokłada: własny sposób wskazywania bieżącego znacznika. `topmost()`
        // i `reach()` mają w bazie zachowanie domyślne — widżet, który niczego nie przewija,
        // dostaje wędrówkę po znacznikach bez pisania czegokolwiek.
        void stand(std::size_t given) { which(given); }

        using ui::MarkedNode<ui::Node>::step;
    };

    Mine one;

    ought(!one.which().has_value());

    one.marks().add(ui::Mark{9, 0, 3, "moja"});
    one.marks().add(ui::Mark{2, 0, 3, "moja"});

    same(one.marks().many(), std::size_t{2});
    same(one.marks()[0].line, std::size_t{2});

    one.stand(1);

    same(*one.which(), std::size_t{1});

    // Zmiana zbioru kasuje kursor — leniwie, przy najbliższym pytaniu.
    one.marks().add(ui::Mark{20, 0, 3, "moja"});

    ought(!one.which().has_value());

    // **Wędrówka też jest w bazie.** Bez kursora `step` naprzód bierze pierwszy znacznik, który
    // nie leży przed ekranem — a widżet, który niczego nie przewija, ma ekran od wiersza zero,
    // więc bierze pierwszy w ogóle.
    ought(one.step(1));
    same(*one.which(), std::size_t{0});

    ought(one.step(1));
    same(*one.which(), std::size_t{1});

    ought(one.step(-1));
    same(*one.which(), std::size_t{0});

    // Poza zbiór się nie wychodzi i nic się przy tym nie rusza.
    ought(!one.step(-1));
    same(*one.which(), std::size_t{0});

    // Krok większy niż jeden przeskakuje po znacznikach, a nie po wierszach.
    ought(one.step(2));
    same(*one.which(), std::size_t{2});
}

// --- Źródło asynchroniczne -----------------------------------------------------------------------

// Podgląd na `Feed` nie czeka na bajty: pierwsza klatka jest pusta, bo okno jest dopiero w drodze,
// a po obrocie pętli wiersze są — bez żadnego „odśwież mnie", bo `Term` maluje po każdym wywołaniu
// zwrotnym z pętli.
TRIAL("Viewer: źródło asynchroniczne maluje pustkę, a po obrocie pętli wiersze") {
    Loop loop;
    Sheet sheet;

    const Bytes raw = bytes(many(100));
    const auto one = std::make_shared<ui::Viewer>();

    one->hold(fed(raw), raw.size());
    sheet.children().add(one);

    const View first = sheet.render({{0, 0}, {40, 10}});

    same(said(first, 0), std::string{});
    ought(!one->lines().has_value());

    ought(turned(loop, sheet, {40, 10}, [&] { return said(sheet.render({{0, 0}, {40, 10}}), 0) == "wiersz 0"; }));

    const View made = sheet.render({{0, 0}, {40, 10}});

    same(said(made, 9), std::string{"wiersz 9"});
    same(*one->at(), std::size_t{0});
    same(*one->whole(), raw.size());
}

// **Rozgrzana szpula.** Kto czytał nagłówek, ma już pierwsze okno — podane ze źródłem, pierwsza
// klatka ma wiersze od razu, bez obrotu pętli (obietnica źródła nie ma jak wrócić przed nim, więc
// wiersze w pierwszej klatce mogły przyjść tylko z głowy). Źródło pyta się wyłącznie o to, czego
// w głowie nie ma — od jej końca wzwyż, dociąganiem z góry i przy przewinięciu; liczba wierszy
// i tak jest nieznana, bo dokument jest dłuższy niż to, co mamy.
TRIAL("Viewer: źródło z głową maluje wiersze od razu, a źródło pyta tylko za głowę") {
    Loop loop;
    Sheet sheet;

    const Bytes raw = bytes(many(100));
    const auto one = std::make_shared<ui::Viewer>();
    const auto asked = std::make_shared<std::vector<std::size_t>>();

    const Feed feed = [raw, asked](std::size_t at, std::size_t most) {
        asked->push_back(at);

        return fed(raw)(at, most);
    };

    // Głowa: pierwsze 40 bajtów — cztery wiersze i kawałek piątego.
    const Bytes head(raw.begin(), raw.begin() + 40);

    one->hold(feed, raw.size(), head);
    sheet.children().add(one);

    const View first = sheet.render({{0, 0}, {40, 2}});

    same(said(first, 0), std::string{"wiersz 0"});
    same(said(first, 1), std::string{"wiersz 1"});
    ought(!one->lines().has_value());
    same(*one->whole(), raw.size());

    // Za głową: przewijanie wiersz po wierszu staje na końcu głowy, aż okno dojdzie z pętli —
    // a każde pytanie do źródła zaczyna się za głową.
    ought(turned(loop, sheet, {40, 2}, [&] {
        one->slide(1);

        return said(sheet.render({{0, 0}, {40, 2}}), 0) == "wiersz 8";
    }));
    ought(!asked->empty());

    for (const std::size_t at : *asked) ought(at >= 40);

    // Głowa dłuższa niż dokument nie kłamie o długości: obcina się do dokumentu.
    Sheet second;

    const auto other = std::make_shared<ui::Viewer>();
    const Bytes tiny = bytes("ab\ncd\n");
    Bytes longer = tiny;

    longer.insert(longer.end(), tiny.begin(), tiny.end());

    other->hold(feed, tiny.size(), longer);
    second.children().add(other);

    (void)second.render({{0, 0}, {40, 4}});
    other->end();

    ought(turned(loop, second, {40, 4}, [&] { return other->lines().has_value(); }));
    same(*other->lines(), std::size_t{2});
}

// `End` na źródle, którego nikt jeszcze nie przeszedł, jest zamiarem: liczba wierszy dochodzi okno
// po oknie, a gdy jest, skok dokańcza się sam — z uchwytu szpuli, nie z drugiego naciśnięcia.
TRIAL("Viewer: End na źródle asynchronicznym dokańcza się, gdy liczba wierszy dojdzie") {
    Loop loop;
    Sheet sheet;

    const Bytes raw = bytes(many(30000));
    const auto one = std::make_shared<ui::Viewer>();

    one->hold(fed(raw), raw.size());
    sheet.children().add(one);

    (void)sheet.render({{0, 0}, {40, 10}});

    one->end();

    ought(!one->lines().has_value());
    same(*one->first(), std::size_t{0});

    ought(turned(loop, sheet, {40, 10}, [&] { return one->lines().has_value() && *one->first() == 29990; }));
    ought(turned(loop, sheet, {40, 10}, [&] { return said(sheet.render({{0, 0}, {40, 10}}), 9) == "wiersz 29999"; }));
}

// Skok po bajcie daleko za oknem też jest zamiarem — szpula dochodzi do bajtu, gdy ma okna, a
// podgląd staje na właściwym wierszu bez ponownego wołania.
TRIAL("Viewer: skok po bajcie na źródle asynchronicznym dokańcza się z uchwytu") {
    Loop loop;
    Sheet sheet;

    const std::string text = many(30000);
    const Bytes raw = bytes(text);
    const auto one = std::make_shared<ui::Viewer>();

    one->hold(fed(raw), raw.size());
    sheet.children().add(one);

    (void)sheet.render({{0, 0}, {40, 10}});

    const std::size_t byte = text.find("wiersz 20000\n");

    one->at(byte);

    ought(turned(loop, sheet, {40, 10}, [&] { return *one->first() == 20000; }));
    ought(turned(loop, sheet, {40, 10}, [&] { return said(sheet.render({{0, 0}, {40, 10}}), 0) == "wiersz 20000"; }));
    same(*one->at(), byte);
}

// Pasek strony liczonej bajtami (U6 w twa): dokument ze źródła, którego wierszy jeszcze nikt nie
// policzył, ma pasek po bajtach — i suwak ciągnięty na koniec skacze na bajt, a nie na wiersz o tym
// numerze.
TRIAL("Viewer: suwak paska po bajtach skacze na bajt") {
    Loop loop;
    Sheet sheet;

    const Bytes raw = bytes(many(100));
    const auto one = std::make_shared<ui::Viewer>(ui::ScrollShape{.scrollbar = true});

    one->hold(fed(raw), raw.size());
    sheet.children().add(one);

    ought(turned(loop, sheet, {40, 10}, [&] { return said(sheet.render({{0, 0}, {40, 10}}), 0).rfind("wiersz 0", 0) == 0; }));
    ought(!one->lines().has_value());

    ui::MouseEvent grab{{39, 1}, ui::Press::down, ui::Mouse::left};
    ui::MouseEvent pull{{39, 20}, ui::Press::move, ui::Mouse::left};
    ui::MouseEvent drop{{39, 20}, ui::Press::up, ui::Mouse::left};

    (void)sheet.dispatch(grab);
    (void)sheet.dispatch(pull);
    (void)sheet.dispatch(drop);

    ought(turned(loop, sheet, {40, 10}, [&] { return said(sheet.render({{0, 0}, {40, 10}}), 0).rfind("wiersz 9", 0) == 0; }));
}
