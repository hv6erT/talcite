#include "proving.hpp"

#include <memory>
#include <string>
#include <vector>

#include "talcite/bytes.hpp"
#include "talcite/encoding.hpp"
#include "talcite/root.hpp"
#include "talcite/viewer.hpp"

using namespace talcite;

namespace {

class Sheet : public ui::Root {};

Bytes lump(const std::string& what) {
    Bytes made;

    made.reserve(what.size());

    for (const char sign : what) made.push_back(static_cast<std::byte>(sign));

    return made;
}

// Wiersz kartki jako napis, z obciętymi spacjami z prawej — inaczej każde porównanie musiałoby
// dopisywać dopełnienie do szerokości okna.
std::string said(const View& one, int y) {
    std::string made;

    for (int x = 0; x < one.size().wide; ++x) {
        const char32_t letter = one.at({x, y}).letter;

        if (letter == 0) continue;

        spelled(letter, made);
    }

    while (!made.empty() && made.back() == ' ') made.pop_back();

    return made;
}

std::shared_ptr<ui::Viewer> shown(Sheet& sheet, const std::string& what) {
    const auto one = std::make_shared<ui::Viewer>();

    one->hold(lump(what));

    sheet.children().add(one);

    return one;
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

        return made;
    },
              10);

    sheet.children().add(one);

    ought(one->held());
    same(*one->lines(), std::size_t{10});
    same(*one->first(), std::size_t{0});
    ought(!one->at().has_value());
    ought(!one->whole().has_value());
}

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

    ought(one->wrapping());

    const View made = sheet.render({{0, 0}, {4, 4}});

    same(said(made, 0), std::string{"aaaa"});
    same(said(made, 1), std::string{"bbbb"});
    same(said(made, 2), std::string{"cccc"});
}

TRIAL("Viewer: bez zawijania wiersz się urywa") {
    Sheet sheet;

    const auto one = shown(sheet, "aaaabbbbcccc\ndrugi");

    one->wrapping(false);

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

TRIAL("Viewer: za ostatni wiersz nie wychodzimy") {
    Sheet sheet;

    const auto one = shown(sheet, many(10));

    (void)sheet.render({{0, 0}, {40, 4}});

    one->slide(1000);

    same(*one->first(), std::size_t{9});
}

// Przy zawijaniu jeden wiersz dokumentu to kilka kroków ekranu, więc krok nie przesuwa numeru
// wiersza za każdym razem.
TRIAL("Viewer: przy zawijaniu krok idzie po kawałkach") {
    Sheet sheet;

    const auto one = shown(sheet, "aaaabbbbcccc\ndrugi");

    (void)sheet.render({{0, 0}, {4, 4}});

    one->slide(1);
    same(*one->first(), std::size_t{0});

    one->slide(1);
    same(*one->first(), std::size_t{0});

    one->slide(1);
    same(*one->first(), std::size_t{1});
}

TRIAL("Viewer: strona przesuwa o wysokość okna") {
    Sheet sheet;

    const auto one = shown(sheet, many(100));

    (void)sheet.render({{0, 0}, {40, 10}});

    one->page(1);

    same(*one->first(), std::size_t{10});
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
    one->wrapping(false);

    same(*one->first(), std::size_t{1});

    const View made = sheet.render({{0, 0}, {4, 4}});

    same(said(made, 0), std::string{"drug"});
}

// --- Pasek przewijania ---------------------------------------------------------------------------

// Pasek zabiera jedną kolumnę z prawej, więc treść dostaje o tyle mniej — i to widać po zawijaniu,
// bo długi wiersz łamie się o kratkę wcześniej.
TRIAL("Viewer: pasek zabiera kolumnę treści") {
    Sheet sheet;

    const auto one = shown(sheet, "aaaabbbb");

    one->progress(true);

    ought(one->progress());

    const View made = sheet.render({{0, 0}, {5, 4}});

    // Ostatnia kolumna należy do paska, więc porównujemy sam początek wiersza.
    same(said(made, 0).substr(0, 4), std::string{"aaaa"});
    same(said(made, 1).substr(0, 4), std::string{"bbbb"});
}

// Bez paska ta sama treść mieści się w pięciu kolumnach.
TRIAL("Viewer: bez paska treść dostaje całą szerokość") {
    Sheet sheet;

    const auto one = shown(sheet, "aaaabbbb");

    ought(!one->progress());

    const View made = sheet.render({{0, 0}, {5, 4}});

    same(said(made, 0), std::string{"aaaab"});
    same(said(made, 1), std::string{"bbb"});
}

// Strzałki paska stoją w ostatniej kolumnie, u góry i u dołu.
TRIAL("Viewer: pasek stoi przy prawej krawędzi") {
    Sheet sheet;

    const auto one = shown(sheet, many(100));

    one->progress(true);

    const View made = sheet.render({{0, 0}, {20, 5}});

    ought(made.at({19, 0}).letter != U' ');
    ought(made.at({19, 4}).letter != U' ');
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

    ought(sheet.deal(down));
    same(*one->first(), std::size_t{1});

    ui::KeyEvent f2{ui::Code::f2};

    ought(!sheet.deal(f2));
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
