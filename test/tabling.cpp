#include "proving.hpp"

#include <memory>
#include <string>
#include <vector>

#include "talcite/encoding.hpp"
#include "talcite/root.hpp"
#include "talcite/table.hpp"

using namespace talcite;

namespace {

class Sheet : public ui::Root {};

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

// Źródło wierszy: nazwa i numer. Oddaje **najwyżej tyle, ile poproszono**, i zero za końcem —
// dokładnie ten kontrakt, którego tabela ma się trzymać.
ui::TableSource numbered(std::size_t much) {
    return [much](std::size_t which, std::size_t most) {
        std::vector<ui::TableRow> made;

        for (std::size_t one = which; one < much && made.size() < most; ++one)
            made.push_back(ui::TableRow{ui::TableCell{"plik" + std::to_string(one), {}},
                                     ui::TableCell{std::to_string(one * 10), {}}});

        return made;
    };
}

std::vector<ui::TableColumn> twofold() {
    return {ui::TableColumn{"Nazwa", 0, ui::Justify::left},
            ui::TableColumn{"Rozmiar", 8, ui::Justify::right}};
}

std::shared_ptr<ui::Table> shown(Sheet& sheet, std::size_t much) {
    const auto one = std::make_shared<ui::Table>();

    one->columns(twofold());
    one->source(numbered(much), much);

    sheet.children().add(one);

    return one;
}

}

// --- Co tabela wie na starcie --------------------------------------------------------------------

TRIAL("Table: pusta nic nie niesie") {
    const ui::Table one;

    ought(!one.sourced());
    same(one.rows(), std::size_t{0});
    ought(!one.which().has_value());
    same(one.first(), std::size_t{0});
}

TRIAL("Table: po podaniu stoi na pierwszym wierszu") {
    Sheet sheet;

    const auto one = shown(sheet, 10);

    ought(one->sourced());
    same(one->rows(), std::size_t{10});
    same(*one->which(), std::size_t{0});
    same(one->first(), std::size_t{0});
}

TRIAL("Table: źródło bez wierszy nie daje bieżącego") {
    Sheet sheet;

    const auto one = shown(sheet, 0);

    ought(!one->which().has_value());

    (void)sheet.render({{0, 0}, {30, 5}});

    same(one->shown(), std::size_t{0});
}

// --- Kolumny -------------------------------------------------------------------------------------

// Kolumna z zerową szerokością bierze resztę okna, a ta ze stałą swoje. Odstęp między nimi jest
// jeden, więc przy oknie na trzydzieści kratek nazwie zostaje dwadzieścia jeden.
TRIAL("Table: kolumna z zerem bierze resztę szerokości") {
    Sheet sheet;

    const auto one = shown(sheet, 3);

    const View made = sheet.render({{0, 0}, {30, 4}});

    same(said(made, 0), std::string{"Nazwa                  Rozmiar"});
    same(said(made, 1), std::string{"plik0                        0"});
    same(said(made, 2), std::string{"plik1                       10"});
}

TRIAL("Table: wyrównanie w prawo dopycha od lewej") {
    Sheet sheet;

    const auto one = std::make_shared<ui::Table>();

    one->heading(false);
    one->columns({ui::TableColumn{"", 6, ui::Justify::right}});
    one->source(
        [](std::size_t which, std::size_t most) {
            std::vector<ui::TableRow> made;

            if (which == 0 && most > 0) made.push_back(ui::TableRow{ui::TableCell{"ab", {}}});

            return made;
        },
        1);

    sheet.children().add(one);

    same(said(sheet.render({{0, 0}, {6, 1}}), 0), std::string{"    ab"});
}

// Okno węższe niż suma kolumn przycina od prawej — panel nie przewija się w poziomie. Przy
// czternastu kratkach na nazwę zostaje dokładnie pięć, czyli tyle, ile ma „plik0".
TRIAL("Table: węższe okno oddaje kolumnie ruchomej mniej") {
    Sheet sheet;

    const auto one = shown(sheet, 3);

    same(said(sheet.render({{0, 0}, {14, 3}}), 1), std::string{"plik0        0"});
}

// Kolumna o stałej szerokości, która sama nie mieści się w oknie, **nie ma prawa zabrać
// wszystkiego**: nazwa jest w panelu treścią, więc zostaje jej co najmniej jedna kratka.
TRIAL("Table: kolumna ruchoma nie znika przy skrajnie wąskim oknie") {
    Sheet sheet;

    const auto one = shown(sheet, 3);

    const View made = sheet.render({{0, 0}, {8, 3}});

    same(made.at({0, 1}).letter, U'p');
}

TRIAL("Table: bez nagłówka pierwszy wiersz stoi na górze") {
    Sheet sheet;

    const auto one = shown(sheet, 3);

    one->heading(false);

    same(said(sheet.render({{0, 0}, {30, 3}}), 0), std::string{"plik0                        0"});
}

// --- Ruch ----------------------------------------------------------------------------------------

TRIAL("Table: krok w dół i w górę chodzi po wierszach") {
    Sheet sheet;

    const auto one = shown(sheet, 10);

    (void)sheet.render({{0, 0}, {30, 5}});

    one->step(1);
    same(*one->which(), std::size_t{1});

    one->step(3);
    same(*one->which(), std::size_t{4});

    one->step(-2);
    same(*one->which(), std::size_t{2});

    one->step(-100);
    same(*one->which(), std::size_t{0});

    one->step(100);
    same(*one->which(), std::size_t{9});
}

// Widok dociąga się za bieżącym wierszem, a nie odwrotnie.
TRIAL("Table: zejście poniżej ekranu przewija widok") {
    Sheet sheet;

    const auto one = shown(sheet, 20);

    (void)sheet.render({{0, 0}, {30, 4}});

    same(one->shown(), std::size_t{3});

    one->step(2);
    same(one->first(), std::size_t{0});

    one->step(1);
    same(one->first(), std::size_t{1});

    (void)sheet.render({{0, 0}, {30, 4}});

    same(said(sheet.render({{0, 0}, {30, 4}}), 1), std::string{"plik1                       10"});
}

TRIAL("Table: koniec staje na ostatnim wierszu") {
    Sheet sheet;

    const auto one = shown(sheet, 20);

    (void)sheet.render({{0, 0}, {30, 4}});

    one->end();

    same(*one->which(), std::size_t{19});
    same(one->first(), std::size_t{17});
}

TRIAL("Table: strona przenosi o ekran") {
    Sheet sheet;

    const auto one = shown(sheet, 30);

    one->focus();

    (void)sheet.render({{0, 0}, {30, 6}});

    ui::KeyEvent down{ui::Code::pagedown};

    ought(sheet.dispatch(down));
    same(*one->which(), std::size_t{5});

    ought(sheet.dispatch(down));
    same(*one->which(), std::size_t{10});

    ui::KeyEvent up{ui::Code::pageup};

    ought(sheet.dispatch(up));
    same(*one->which(), std::size_t{5});
}

// Kółko przesuwa **sam widok**, a wiersza bieżącego nie rusza — tak samo jak w liście.
TRIAL("Table: kółko przewija widok, a wiersza nie rusza") {
    Sheet sheet;

    const auto one = shown(sheet, 30);

    (void)sheet.render({{0, 0}, {30, 6}});

    ui::WheelEvent what{{2, 2}, -1};

    ought(sheet.dispatch(what));

    same(one->first(), std::size_t{3});
    same(*one->which(), std::size_t{0});
}

TRIAL("Table: kółko na końcu przewijania oddaje zdarzenie wyżej") {
    Sheet sheet;

    const auto one = shown(sheet, 30);

    (void)sheet.render({{0, 0}, {30, 6}});

    ui::WheelEvent up{{2, 2}, 1};

    ought(!sheet.dispatch(up));
}

TRIAL("Table: ruch zawiadamia zdarzeniem, a nie wywołaniem zwrotnym") {
    Sheet sheet;

    const auto one = shown(sheet, 10);

    int moves = 0;

    (void)one->on<ui::ScrollEvent>([&moves](ui::ScrollEvent&) {
        ++moves;

        return false;
    });

    (void)sheet.render({{0, 0}, {30, 5}});

    one->step(1);
    same(moves, 1);

    one->step(0);
    same(moves, 1);

    one->first(3);
    same(moves, 2);
}

// --- Kliknięcie ----------------------------------------------------------------------------------

TRIAL("Table: kliknięcie stawia wiersz bieżący pod nagłówkiem") {
    Sheet sheet;

    const auto one = shown(sheet, 10);

    (void)sheet.render({{0, 0}, {30, 6}});

    ui::ClickEvent what{{4, 3}, ui::Mouse::left};

    ought(sheet.dispatch(what));
    same(*one->which(), std::size_t{2});
}

// Kliknięcie w sam nagłówek nie jest kliknięciem w wiersz.
TRIAL("Table: kliknięcie w nagłówek nie rusza wiersza") {
    Sheet sheet;

    const auto one = shown(sheet, 10);

    (void)sheet.render({{0, 0}, {30, 6}});

    one->step(4);

    ui::ClickEvent what{{4, 0}, ui::Mouse::left};

    ought(sheet.dispatch(what));
    same(*one->which(), std::size_t{4});
}

TRIAL("Table: kliknięcie poniżej ostatniego wiersza nic nie zmienia") {
    Sheet sheet;

    const auto one = shown(sheet, 2);

    (void)sheet.render({{0, 0}, {30, 8}});

    ui::ClickEvent what{{4, 6}, ui::Mouse::left};

    ought(sheet.dispatch(what));
    same(*one->which(), std::size_t{0});
}

// Dwuklik na wierszu stawia go bieżącym i idzie wyżej drogą widżetów — co znaczy, wie aplikacja; na
// nagłówku i pod ostatnim wierszem tabela go bierze i dalej nie idzie (U5).
TRIAL("Table: dwuklik na wierszu idzie wyżej, na nagłówku i pustce — nie") {
    struct Heard : ui::Root {
        int many = 0;

        bool handle(ui::DoubleClickEvent&) override {
            ++many;

            return true;
        }
    };

    Heard sheet;

    const auto one = std::make_shared<ui::Table>();

    one->columns(twofold());
    one->source(numbered(3), 3);

    sheet.children().add(one);

    (void)sheet.render({{0, 0}, {30, 8}});

    ui::DoubleClickEvent row{{4, 3}, ui::Mouse::left};

    ought(sheet.dispatch(row));
    same(*one->which(), std::size_t{2});
    same(sheet.many, 1);

    ui::DoubleClickEvent head{{4, 0}, ui::Mouse::left};
    ui::DoubleClickEvent below{{4, 6}, ui::Mouse::left};

    ought(sheet.dispatch(head));
    ought(sheet.dispatch(below));
    same(sheet.many, 1);
    same(*one->which(), std::size_t{2});
}

// --- Zaznaczanie ---------------------------------------------------------------------------------

TRIAL("Table: zaznaczanie jest czym innym niż wiersz bieżący") {
    Sheet sheet;

    const auto one = shown(sheet, 5);

    ought(!one->chosen().contains(0));

    one->chosen().add(2);
    one->chosen().add(4);

    ought(one->chosen().contains(2));
    ought(one->chosen().contains(4));
    ought(!one->chosen().contains(3));
    same(*one->which(), std::size_t{0});

    const std::vector<std::size_t> want{2, 4};

    same(one->chosen().all(), want);

    one->chosen().clear();

    ought(one->chosen().all().empty());
}

TRIAL("Table: zaznaczenie poza zakresem czeka, a rysowanie je przycina") {
    Sheet sheet;

    const auto one = shown(sheet, 3);

    one->chosen().add(9);

    // **Zbiór nie zna liczby wierszy**, więc zapamiętuje — i to jest zamierzone: zaznaczenie
    // przeżywa do chwili, w której tabela urośnie na tyle, żeby ten wiersz istniał.
    ought(one->chosen().contains(9));

    // Przycina tabela, przy najbliższym rysowaniu — tą samą leniwą drogą, którą historia przycina
    // swoją listę, a strona kasuje kursor wędrówki po znacznikach.
    (void)sheet.render({{0, 0}, {30, 5}});

    ought(!one->chosen().contains(9));
    ought(one->chosen().all().empty());
}

// Liczba zaznaczonych idzie obok bitów: drugie dodanie tego samego i zdjęcie niezaznaczonego jej nie
// ruszają, przycięcie przy rysowaniu zdejmuje tylko to, co odcięło, a `clear` zeruje.
TRIAL("Table: liczba zaznaczonych nadąża za dodaniem, zdjęciem i przycięciem") {
    Sheet sheet;

    const auto one = shown(sheet, 3);

    ought(one->chosen().empty());

    one->chosen().add(1);
    one->chosen().add(1);
    one->chosen().add(2);
    one->chosen().add(7);
    one->chosen().add(9);

    same(one->chosen().many(), std::size_t{4});

    one->chosen().remove(0);
    one->chosen().remove(20);
    one->chosen().remove(2);
    one->chosen().remove(2);

    same(one->chosen().many(), std::size_t{3});

    (void)sheet.render({{0, 0}, {30, 5}});

    same(one->chosen().many(), std::size_t{1});
    ought(!one->chosen().empty());

    one->chosen().clear();

    same(one->chosen().many(), std::size_t{0});
    ought(one->chosen().empty());
}

// Insert zaznacza i schodzi niżej — odruch z panelu.
// **Insertu tabela nie bierze.** Stało tu zaznaczanie ze zejściem o wiersz niżej — jedyne miejsce
// w bibliotece, gdzie `handle` zmieniał dane, a nie tylko przesuwał kursor, i którego wołający nie
// miał jak odwołać: przechwycenie „wyżej" nie działa, bo tabela leży bliżej ogniska.
TRIAL("Table: insert przechodzi wyżej, a zaznaczenia nie rusza") {
    Sheet sheet;

    const auto one = shown(sheet, 5);

    one->focus();

    (void)sheet.render({{0, 0}, {30, 6}});

    ui::KeyEvent what{ui::Code::insert};

    ought(!sheet.dispatch(what));

    ought(!one->chosen().contains(0));
    same(*one->which(), std::size_t{0});
}

// Kto chce ten odruch, wiesza uchwyt i woła `mark` sam — i wtedy działa, bo uchwyt widzi klawisz
// niezależnie od tego, co robi `handle`.
TRIAL("Table: zaznaczanie Insertem da się dołożyć uchwytem") {
    Sheet sheet;

    const auto one = shown(sheet, 5);

    one->focus();

    (void)sheet.render({{0, 0}, {30, 6}});

    const std::weak_ptr<ui::Table> weak = one;

    (void)one->on<ui::KeyEvent>([weak](ui::KeyEvent& what) {
        if (what.key().code() != ui::Code::insert) return false;

        const auto mine = weak.lock();

        if (!mine) return false;

        const std::size_t at = mine->which().value_or(0);

        if (mine->chosen().contains(at))
            mine->chosen().remove(at);
        else
            mine->chosen().add(at);

        mine->step(1);

        return true;
    });

    ui::KeyEvent what{ui::Code::insert};

    ought(sheet.dispatch(what));

    ought(one->chosen().contains(0));
    same(*one->which(), std::size_t{1});
}

// --- Role ----------------------------------------------------------------------------------------

TRIAL("Table: wiersz bieżący, zaznaczony i zwykły mają różne role") {
    Sheet sheet;

    const auto one = shown(sheet, 4);

    one->chosen().add(1);

    const View made = sheet.render({{0, 0}, {30, 5}});

    // Nagłówek i wiersz bieżący na odwrót, zaznaczony wytłuszczony, reszta zwykła.
    ought(made.at({0, 0}).style.reverse);
    ought(made.at({0, 1}).style.reverse);
    ought(made.at({0, 2}).style.bold);
    ought(!made.at({0, 2}).style.reverse);
    ought(!made.at({0, 3}).style.bold);
}

TRIAL("Table: rola komórki nakłada się na rolę wiersza") {
    Sheet sheet;

    const auto one = std::make_shared<ui::Table>();

    one->heading(false);
    one->columns({ui::TableColumn{"", 4, ui::Justify::left}, ui::TableColumn{"", 4, ui::Justify::left}});
    one->source(
        [](std::size_t which, std::size_t most) {
            std::vector<ui::TableRow> made;

            if (which == 0 && most > 0)
                made.push_back(ui::TableRow{ui::TableCell{"aa", "talcite.table.mark"}, ui::TableCell{"bb", {}}});

            return made;
        },
        1);

    sheet.children().add(one);

    const View made = sheet.render({{0, 0}, {9, 1}});

    // Wiersz jest bieżący, więc na odwrót; pierwsza komórka dokłada do tego wytłuszczenie.
    ought(made.at({0, 0}).style.reverse);
    ought(made.at({0, 0}).style.bold);
    ought(made.at({5, 0}).style.reverse);
    ought(!made.at({5, 0}).style.bold);
}

// --- Odświeżenie ---------------------------------------------------------------------------------

// `again` wyrzuca zapamiętane wiersze i pyta źródło od nowa, ale **zaznaczeń nie czyści**:
// to wołający wie, czy numery wierszy dalej znaczą to samo.
TRIAL("Table: again pyta źródło od nowa i zostawia zaznaczenia") {
    Sheet sheet;

    const auto said = std::make_shared<std::string>("stare");

    const auto one = std::make_shared<ui::Table>();

    one->heading(false);
    one->columns({ui::TableColumn{"", 0, ui::Justify::left}});
    one->source(
        [said](std::size_t which, std::size_t most) {
            std::vector<ui::TableRow> made;

            if (which == 0 && most > 0) made.push_back(ui::TableRow{ui::TableCell{*said, {}}});

            return made;
        },
        1);

    sheet.children().add(one);

    (void)sheet.render({{0, 0}, {10, 1}});

    one->chosen().add(0);

    *said = "nowe";

    one->refresh();

    const View made = sheet.render({{0, 0}, {10, 1}});

    ought(one->chosen().contains(0));

    std::string got;

    for (int x = 0; x < 4; ++x) utf8::encode(made.at({x, 0}).letter, got);

    same(got, std::string{"nowe"});
}

// Podanie nowego źródła zaczyna od zera: nowy dokument to nowe zaznaczenia i nowe położenie.
TRIAL("Table: nowe źródło zeruje położenie i zaznaczenia") {
    Sheet sheet;

    const auto one = shown(sheet, 10);

    (void)sheet.render({{0, 0}, {30, 5}});

    one->end();
    one->chosen().add(3);

    one->source(numbered(4), 4);

    same(*one->which(), std::size_t{0});
    same(one->first(), std::size_t{0});
    ought(one->chosen().all().empty());
    same(one->rows(), std::size_t{4});
}

// --- Pasek ---------------------------------------------------------------------------------------

TRIAL("Table: pasek zabiera kolumnę treści") {
    Sheet sheet;

    const auto one = shown(sheet, 40);

    one->heading(false);

    // Bez paska liczba wyrównana do prawej kończy się w ostatniej kolumnie okna.
    same(sheet.render({{0, 0}, {30, 3}}).at({29, 0}).letter, U'0');

    one->scrollbar(true);

    const View made = sheet.render({{0, 0}, {30, 3}});

    // Z paskiem treść zwęża się o kratkę, a na jej miejscu staje pasek.
    same(made.at({28, 0}).letter, U'0');
    ought(made.at({29, 0}).letter != U' ');
}

// Pasek stoi tylko wtedy, gdy wierszy jest więcej, niż widać; kolumna na niego zostaje, żeby
// kolumny tabeli nie skakały, gdy lista urośnie.
TRIAL("Table: pasek tylko przy wierszach, których nie widać") {
    Sheet sheet;

    const auto one = shown(sheet, 3);

    one->heading(false);
    one->scrollbar(true);

    const View fits = sheet.render({{0, 0}, {30, 3}});

    same(fits.at({29, 0}).letter, U' ');
    same(fits.at({28, 0}).letter, U'0');

    Sheet other;

    const auto more = shown(other, 4);

    more->heading(false);
    more->scrollbar(true);

    ought(other.render({{0, 0}, {30, 3}}).at({29, 0}).letter != U' ');

    // Nagłówek zabiera wiersz: trzy wiersze pod nagłówkiem w oknie na trzy już się nie mieszczą.
    Sheet headed;

    const auto titled = shown(headed, 3);

    titled->scrollbar(true);

    ought(headed.render({{0, 0}, {30, 3}}).at({29, 0}).letter != U' ');
}

// --- Co idzie wyżej ------------------------------------------------------------------------------

// Tabela nie zna pojęcia „wybrano wiersz": w panelu Enter wchodzi do katalogu, a w oknie
// dialogowym naciska przycisk podstawowy. Litery też idą wyżej — do szybkiego szukania.
TRIAL("Table: enter i litery przechodzą wyżej") {
    Sheet sheet;

    const auto one = shown(sheet, 5);

    one->focus();

    ui::KeyEvent enter{ui::Code::enter};

    ought(!sheet.dispatch(enter));

    ui::KeyEvent letter{U'a'};

    ought(!sheet.dispatch(letter));

    ui::KeyEvent f2{ui::Code::f2};

    ought(!sheet.dispatch(f2));
}

TRIAL("Table: nie przyjmuje dzieci") {
    ui::Table one;

    same(one.capacity(), 0);
}

// Reszta słownika zaznaczenia: ile, zdjęcie jednego i pustość. Zaznaczenie było kiedyś bazą
// ChosenNode „na zapas" dla cudzych widżetów wierszowych; drugi użytkownik się nie pojawił, więc
// tabela trzyma zbiór wprost, a ta próba sprawdza go na niej.
TRIAL("Table: zaznaczenie liczy, zdejmuje pojedynczo i wie, że jest puste") {
    Sheet sheet;

    const auto one = shown(sheet, 10);

    ought(one->chosen().empty());

    one->chosen().add(1);
    one->chosen().add(4);
    one->chosen().add(9);

    same(one->chosen().many(), std::size_t{3});
    ought(!one->chosen().empty());

    one->chosen().remove(4);

    ought(!one->chosen().contains(4));
    same(one->chosen().all(), (std::vector<std::size_t>{1, 9}));
}
