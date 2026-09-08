#include "proving.hpp"

#include <memory>
#include <random>
#include <string>
#include <vector>

#ifndef _WIN32

#include <fcntl.h>
#include <unistd.h>

#endif

#include "talcite/bytes.hpp"
#include "talcite/editor.hpp"
#include "talcite/encoding.hpp"
#include "talcite/root.hpp"

using namespace talcite;

namespace {

class Sheet : public ui::Root {};

Bytes lump(const std::string& what) {
    Bytes made;

    made.reserve(what.size());

    for (const char sign : what) made.push_back(static_cast<std::byte>(sign));

    return made;
}

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

// Cały dokument — przez zaznaczenie, bo edytor nie ma innej drogi i mieć nie musi: aplikacja
// zapisująca plik i tak weźmie go zakresami.
std::string whole(ui::Editor& one) {
    const std::size_t was = one.cursor();

    one.select(0, one.whole().value_or(0));

    const std::string made = one.selection();

    one.cursor(was);

    return made;
}

std::string many(int lines) {
    std::string made;

    for (int one = 0; one < lines; ++one) made += "l" + std::to_string(one) + "\n";

    made.pop_back();

    return made;
}

std::shared_ptr<ui::Editor> shown(Sheet& sheet, const std::string& what) {
    const auto one = std::make_shared<ui::Editor>();

    one->hold(lump(what));

    sheet.children().add(one);

    return one;
}

}

// --- Co edytor wie na starcie --------------------------------------------------------------------

TRIAL("Editor: pusty nic nie niesie") {
    const ui::Editor one;

    ought(!one.held());
    ought(!one.dirty());
    ought(!one.readonly());
    same(one.cursor(), std::size_t{0});
}

TRIAL("Editor: po podaniu zna wiersze i stoi na początku") {
    Sheet sheet;

    const auto one = shown(sheet, "a\nbb\nccc");

    ought(one->held());
    ought(!one->dirty());
    same(*one->lines(), std::size_t{3});
    same(*one->whole(), std::size_t{8});
    same(one->cursor(), std::size_t{0});
    same(whole(*one), std::string{"a\nbb\nccc"});
}

// --- Zmiany --------------------------------------------------------------------------------------

TRIAL("Editor: wpisanie znaku przesuwa kursor i brudzi dokument") {
    Sheet sheet;

    const auto one = shown(sheet, "abc");

    one->cursor(1);
    one->insert("X");

    same(whole(*one), std::string{"aXbc"});
    same(one->cursor(), std::size_t{2});
    ought(one->dirty());
}

// Enter dzieli wiersz, więc liczba wierszy rośnie — i to jest ta poprawka, którą szpula dostaje
// z `changed`, zamiast liczyć dokument od nowa.
TRIAL("Editor: enter dokłada wiersz") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    same(*one->lines(), std::size_t{1});

    one->cursor(3);
    one->insert("\n");

    same(*one->lines(), std::size_t{2});
    same(whole(*one), std::string{"abc\ndef"});
}

TRIAL("Editor: kasowanie wstecz zabiera znak przed kursorem") {
    Sheet sheet;

    const auto one = shown(sheet, "abcd");

    one->cursor(3);
    one->back_erase(1);

    same(whole(*one), std::string{"abd"});
    same(one->cursor(), std::size_t{2});
}

TRIAL("Editor: kasowanie w przód zabiera znak za kursorem") {
    Sheet sheet;

    const auto one = shown(sheet, "abcd");

    one->cursor(1);
    one->front_erase(1);

    same(whole(*one), std::string{"acd"});
    same(one->cursor(), std::size_t{1});
}

TRIAL("Editor: skasowanie entera scala wiersze") {
    Sheet sheet;

    const auto one = shown(sheet, "abc\ndef");

    same(*one->lines(), std::size_t{2});

    one->cursor(4);
    one->back_erase(1);

    same(*one->lines(), std::size_t{1});
    same(whole(*one), std::string{"abcdef"});
}

TRIAL("Editor: tylko do odczytu nie przyjmuje zmian") {
    Sheet sheet;

    const auto one = shown(sheet, "abc");

    one->readonly(true);
    one->insert("X");
    one->back_erase(1);

    same(whole(*one), std::string{"abc"});
    ought(!one->dirty());
}

// --- Zaznaczenie ---------------------------------------------------------------------------------

TRIAL("Editor: zaznaczenie oddaje swój tekst") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    ought(!one->selected());

    one->select(2, 5);

    ought(one->selected());
    same(one->selection(), std::string{"cde"});
    same(one->anchor(), std::size_t{2});
    same(one->cursor(), std::size_t{5});
}

TRIAL("Editor: wpisanie na zaznaczeniu zastępuje je") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    one->select(2, 5);
    one->insert("X");

    same(whole(*one), std::string{"abXf"});
    ought(!one->selected());
}

TRIAL("Editor: kasowanie na zaznaczeniu zabiera całe") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    one->select(1, 4);
    one->back_erase(1);

    same(whole(*one), std::string{"aef"});
    same(one->cursor(), std::size_t{1});
}

// Zaznaczenie rysuje się znacznikami, po jednym na wiersz — ale tylko dla tego, co widać.
TRIAL("Editor: zaznaczenie widać na ekranie") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    one->select(2, 5);

    const View made = sheet.render({{0, 0}, {20, 2}});

    ought(!made.at({1, 0}).style.reverse);
    ought(made.at({2, 0}).style.reverse);
    ought(made.at({4, 0}).style.reverse);
    ought(!made.at({5, 0}).style.reverse);
}

TRIAL("Editor: zaznaczenie przez dwa wiersze daje znacznik na każdym") {
    Sheet sheet;

    const auto one = shown(sheet, "abc\ndef");

    one->select(1, 6);

    const View made = sheet.render({{0, 0}, {20, 3}});

    ought(!made.at({0, 0}).style.reverse);
    ought(made.at({1, 0}).style.reverse);
    ought(made.at({2, 0}).style.reverse);
    ought(made.at({0, 1}).style.reverse);
    ought(made.at({1, 1}).style.reverse);
    ought(!made.at({2, 1}).style.reverse);
}

// --- Kursor na ekranie ---------------------------------------------------------------------------

TRIAL("Editor: kursor stoi tam, gdzie wskazuje bajt") {
    Sheet sheet;

    const auto one = shown(sheet, "abc\ndef");

    one->cursor(5);

    (void)sheet.render({{0, 0}, {20, 3}});

    const std::optional<ui::Spot> at = one->caret();

    ought(at.has_value());
    same(at->x, 1);
    same(at->y, 1);
}

TRIAL("Editor: kursor za ostatnim znakiem wiersza też ma miejsce") {
    Sheet sheet;

    const auto one = shown(sheet, "abc\ndef");

    one->cursor(3);

    (void)sheet.render({{0, 0}, {20, 3}});

    const std::optional<ui::Spot> at = one->caret();

    ought(at.has_value());
    same(at->x, 3);
    same(at->y, 0);
}

// --- Klawisze ------------------------------------------------------------------------------------

TRIAL("Editor: litera trafia do dokumentu") {
    Sheet sheet;

    const auto one = shown(sheet, "ab");

    one->focus();

    ui::KeyEvent what{U'X'};

    ought(sheet.deal(what));
    same(whole(*one), std::string{"Xab"});
}

TRIAL("Editor: strzałki chodzą po znakach i po wierszach") {
    Sheet sheet;

    const auto one = shown(sheet, "abc\ndef");

    one->focus();

    (void)sheet.render({{0, 0}, {20, 3}});

    ui::KeyEvent right{ui::Code::right};

    ought(sheet.deal(right));
    same(one->cursor(), std::size_t{1});

    ui::KeyEvent down{ui::Code::down};

    ought(sheet.deal(down));
    same(one->cursor(), std::size_t{5});

    ui::KeyEvent left{ui::Code::left};

    ought(sheet.deal(left));
    same(one->cursor(), std::size_t{4});
}

// Znak wielobajtowy kasuje się w całości — inaczej Backspace rozcinałby ogonek na pół i zostawiał
// śmieć, którego nie da się już wpisać z powrotem.
TRIAL("Editor: kasowanie zabiera cały znak, nie jeden bajt") {
    Sheet sheet;

    const auto one = shown(sheet, "a\xc4\x85"
                                  "b");

    one->focus();

    one->cursor(3);

    ui::KeyEvent back{ui::Code::backspace};

    ought(sheet.deal(back));
    same(whole(*one), std::string{"ab"});
    same(one->cursor(), std::size_t{1});
}

// --- Cofanie -------------------------------------------------------------------------------------

TRIAL("Editor: świeży dokument nie ma czego cofać") {
    Sheet sheet;

    const auto one = shown(sheet, "abc");

    ought(!one->undoable());
    ought(!one->redoable());
    ought(!one->dirty());

    one->undo();
    one->redo();

    same(whole(*one), std::string{"abc"});
}

TRIAL("Editor: cofnięcie oddaje dokument sprzed zmiany, ponowienie go przywraca") {
    Sheet sheet;

    const auto one = shown(sheet, "abc");

    one->cursor(1);
    one->insert("X");

    same(whole(*one), std::string{"aXbc"});
    ought(one->undoable());
    ought(!one->redoable());

    one->undo();

    same(whole(*one), std::string{"abc"});
    ought(!one->undoable());
    ought(one->redoable());

    one->redo();

    same(whole(*one), std::string{"aXbc"});
    ought(one->undoable());
    ought(!one->redoable());
}

// Pisany wyraz zbiera się w jeden wpis — inaczej cofanie chodziłoby po literze, a pięć tysięcy
// znaków dałoby pięć tysięcy kroków.
TRIAL("Editor: wpisany wyraz cofa się w całości") {
    Sheet sheet;

    const auto one = shown(sheet, "");

    one->insert("a");
    one->insert("b");
    one->insert("c");

    same(whole(*one), std::string{"abc"});

    one->undo();

    same(whole(*one), std::string{});
    ought(!one->undoable());
}

TRIAL("Editor: spacja kończy wyraz, więc cofa się osobno") {
    Sheet sheet;

    const auto one = shown(sheet, "");

    for (const char sign : std::string{"ala "}) one->insert(std::string{sign});
    for (const char sign : std::string{"ma"}) one->insert(std::string{sign});

    same(whole(*one), std::string{"ala ma"});

    one->undo();

    same(whole(*one), std::string{"ala "});

    one->undo();

    same(whole(*one), std::string{});
}

TRIAL("Editor: ruch kursora zamyka wyraz") {
    Sheet sheet;

    const auto one = shown(sheet, "xy");

    one->cursor(2);
    one->insert("a");
    one->insert("b");

    one->cursor(0);
    one->insert("Z");

    same(whole(*one), std::string{"Zxyab"});

    one->undo();

    same(whole(*one), std::string{"xyab"});

    one->undo();

    same(whole(*one), std::string{"xy"});
}

TRIAL("Editor: enter jest osobnym krokiem") {
    Sheet sheet;

    const auto one = shown(sheet, "");

    one->insert("a");
    one->insert("\n");
    one->insert("b");

    same(*one->lines(), std::size_t{2});

    one->undo();

    same(whole(*one), std::string{"a\n"});

    one->undo();

    same(whole(*one), std::string{"a"});
    same(*one->lines(), std::size_t{1});
}

// Wpisanie na zaznaczeniu to **jedna** zmiana. Gdyby było kasowaniem i wpisaniem po nim, jedno
// cofnięcie oddałoby dokument bez zaznaczonego kawałka zamiast tego, co w nim stało.
TRIAL("Editor: wpisanie na zaznaczeniu cofa się jednym ruchem") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    one->select(2, 5);
    one->insert("X");

    same(whole(*one), std::string{"abXf"});

    one->undo();

    same(whole(*one), std::string{"abcdef"});
    ought(!one->undoable());
}

TRIAL("Editor: trzymany backspace zbiera się w jeden krok") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    one->cursor(6);
    one->back_erase(1);
    one->back_erase(1);
    one->back_erase(1);

    same(whole(*one), std::string{"abc"});

    one->undo();

    same(whole(*one), std::string{"abcdef"});
    same(one->cursor(), std::size_t{6});
}

TRIAL("Editor: trzymany delete zbiera się w jeden krok") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    one->cursor(1);
    one->front_erase(1);
    one->front_erase(1);

    same(whole(*one), std::string{"adef"});

    one->undo();

    same(whole(*one), std::string{"abcdef"});
}

// Nowa zmiana po cofnięciu ucina gałąź ponowienia — nie ma już dokąd ponawiać.
TRIAL("Editor: zmiana po cofnięciu ucina ponowienie") {
    Sheet sheet;

    const auto one = shown(sheet, "abc");

    one->cursor(3);
    one->insert("X");
    one->undo();

    ought(one->redoable());

    one->cursor(0);
    one->insert("Y");

    ought(!one->redoable());
    same(whole(*one), std::string{"Yabc"});
}

// Brudny liczy się z dziennika, nie z kołdry: cofnięcie wszystkiego wraca do tego samego miejsca
// w dzienniku, więc dokument jest znowu czysty.
TRIAL("Editor: cofnięcie wszystkiego czyści dokument") {
    Sheet sheet;

    const auto one = shown(sheet, "abc");

    one->cursor(1);
    one->insert("X");

    ought(one->dirty());

    one->undo();

    ought(!one->dirty());

    one->redo();

    ought(one->dirty());

    one->clean();

    ought(!one->dirty());

    one->undo();

    ought(one->dirty());
}

TRIAL("Editor: cofnięcie stawia kursor tam, gdzie stał przed zmianą") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    one->cursor(4);
    one->insert("XY");

    same(one->cursor(), std::size_t{6});

    one->undo();

    same(one->cursor(), std::size_t{4});
}

TRIAL("Editor: cofnięcie odtwarza liczbę wierszy") {
    Sheet sheet;

    const auto one = shown(sheet, "abc\ndef\nghi");

    same(*one->lines(), std::size_t{3});

    one->cursor(3);
    one->back_erase(1);

    same(*one->lines(), std::size_t{3});
    same(whole(*one), std::string{"ab\ndef\nghi"});

    one->cursor(2);
    one->front_erase(1);

    same(*one->lines(), std::size_t{2});

    one->undo();

    same(*one->lines(), std::size_t{3});
    same(whole(*one), std::string{"ab\ndef\nghi"});
}

// Wiersz nie bierze się z entera, tylko z tego, czy po nim coś jest: „abc" i „abc\n" to jeden
// wiersz, a pusty dokument nie ma żadnego. Liczona samymi enterami poprawka rozjeżdżała się
// na obu tych krańcach.
TRIAL("Editor: enter na końcu dokumentu nie dokłada wiersza") {
    Sheet sheet;

    const auto one = shown(sheet, "abc");

    same(*one->lines(), std::size_t{1});

    one->cursor(3);
    one->insert("\n");

    same(*one->lines(), std::size_t{1});

    one->insert("d");

    same(*one->lines(), std::size_t{2});

    one->undo();
    one->undo();

    same(*one->lines(), std::size_t{1});
    same(whole(*one), std::string{"abc"});
}

TRIAL("Editor: pusty dokument nie ma wiersza, a jeden znak już ma") {
    Sheet sheet;

    const auto one = shown(sheet, "");

    same(*one->lines(), std::size_t{0});

    one->insert("a");

    same(*one->lines(), std::size_t{1});

    one->back_erase(1);

    same(*one->lines(), std::size_t{0});
}

TRIAL("Editor: tylko do odczytu nie cofa") {
    Sheet sheet;

    const auto one = shown(sheet, "abc");

    one->cursor(1);
    one->insert("X");
    one->readonly(true);

    ought(!one->undoable());

    one->undo();

    same(whole(*one), std::string{"aXbc"});
}

// Losowy ciąg zmian cofnięty do końca musi dać oryginał co do bajtu — to jedyny sposób, żeby
// złapać błąd w odwracaniu zmiany, którego nie widać na żadnym pojedynczym przypadku.
TRIAL("Editor: losowy ciąg zmian cofa się do oryginału") {
    std::mt19937 gen(11);

    for (int round = 0; round < 60; ++round) {
        Sheet sheet;

        const std::string was = "pierwszy\ndrugi\ntrzeci\nczwarty";

        const auto one = shown(sheet, was);

        std::vector<std::string> steps;

        for (int step = 0; step < 12; ++step) {
            const std::string before = whole(*one);

            const std::size_t much = one->whole().value_or(0);

            one->cursor(much == 0 ? 0 : gen() % (much + 1));

            if (gen() % 2 || much == 0) {
                std::string what;

                const int size = 1 + static_cast<int>(gen() % 4);

                for (int which = 0; which < size; ++which)
                    what.push_back(gen() % 7 == 0 ? '\n' : static_cast<char>('A' + gen() % 26));

                one->insert(what);
            } else if (gen() % 2) {
                one->back_erase(1 + gen() % 3);
            } else {
                one->front_erase(1 + gen() % 3);
            }

            // Krok, który niczego nie zmienił — kasowanie na krańcu — nie zostawia wpisu
            // w dzienniku, więc nie ma go też czym cofać.
            if (whole(*one) != before) steps.push_back(before);
        }

        while (!steps.empty()) {
            one->undo();

            same(whole(*one), steps.back());

            steps.pop_back();
        }

        ought(!one->undoable());
        ought(!one->dirty());
        same(whole(*one), was);
    }
}

// Skok po wyrazach chodzi ta sama regula, co szukanie calych wyrazow i dwuklik — jedna definicja
// wyrazu w calej bibliotece.
TRIAL("Editor: ctrl ze strzałką idzie po wyrazach") {
    Sheet sheet;

    const auto one = shown(sheet, "ala ma kota");

    one->focus();

    ui::KeyEvent right{ui::Key{ui::Code::right, ui::Mods{true, false, false}}};

    ought(sheet.deal(right));
    same(one->cursor(), std::size_t{4});

    ought(sheet.deal(right));
    same(one->cursor(), std::size_t{7});

    ought(sheet.deal(right));
    same(one->cursor(), std::size_t{11});

    ui::KeyEvent left{ui::Key{ui::Code::left, ui::Mods{true, false, false}}};

    ought(sheet.deal(left));
    same(one->cursor(), std::size_t{7});

    ought(sheet.deal(left));
    same(one->cursor(), std::size_t{4});

    ought(sheet.deal(left));
    same(one->cursor(), std::size_t{0});
}

TRIAL("Editor: skok po wyrazach przechodzi między wierszami") {
    Sheet sheet;

    const auto one = shown(sheet, "abc\ndef");

    one->focus();

    one->cursor(3);

    ui::KeyEvent right{ui::Key{ui::Code::right, ui::Mods{true, false, false}}};

    ought(sheet.deal(right));
    same(one->cursor(), std::size_t{4});

    ui::KeyEvent left{ui::Key{ui::Code::left, ui::Mods{true, false, false}}};

    ought(sheet.deal(left));
    same(one->cursor(), std::size_t{3});
}

TRIAL("Editor: skok po wyrazach nie rozcina ogonka") {
    Sheet sheet;

    const auto one = shown(sheet, "k\xc4\x85t ma");

    one->focus();

    ui::KeyEvent right{ui::Key{ui::Code::right, ui::Mods{true, false, false}}};

    ought(sheet.deal(right));
    same(one->cursor(), std::size_t{5});
}

TRIAL("Editor: ctrl ze strzałką i shiftem ciągnie zaznaczenie") {
    Sheet sheet;

    const auto one = shown(sheet, "ala ma kota");

    one->focus();

    ui::KeyEvent right{ui::Key{ui::Code::right, ui::Mods{true, false, true}}};

    ought(sheet.deal(right));
    ought(sheet.deal(right));

    same(one->anchor(), std::size_t{0});
    same(one->selection(), std::string{"ala ma "});
}

TRIAL("Editor: skok po wyrazach na krańcach stoi w miejscu") {
    Sheet sheet;

    const auto one = shown(sheet, "ala");

    one->focus();

    ui::KeyEvent left{ui::Key{ui::Code::left, ui::Mods{true, false, false}}};

    ought(sheet.deal(left));
    same(one->cursor(), std::size_t{0});

    one->cursor(3);

    ui::KeyEvent right{ui::Key{ui::Code::right, ui::Mods{true, false, false}}};

    ought(sheet.deal(right));
    same(one->cursor(), std::size_t{3});
}

// --- Schowek -------------------------------------------------------------------------------------

namespace {

// Kopiowanie z zatkanym wyjsciem: na POSIX-ie schowek pisze OSC 52 prosto na wyjscie, wiec bez
// tego kazda proba wysypywalaby sekwencje sterujaca w wynik przebiegu.
template <typename One>
void aside(One one) {
#ifdef _WIN32

    one();

#else

    const int was = dup(STDOUT_FILENO);
    const int sink = open("/dev/null", O_WRONLY);

    if (sink >= 0) (void)dup2(sink, STDOUT_FILENO);

    one();

    if (sink >= 0) {
        (void)dup2(was, STDOUT_FILENO);

        close(sink);
    }

    close(was);

#endif
}

}

TRIAL("Editor: kopiowanie i wklejanie chodzą przez schowek") {
    Sheet sheet;

    const auto one = shown(sheet, "ala ma kota");

    one->select(0, 3);

    aside([one] { ought(one->copy()); });

    one->cursor(11);
    one->insert(" ");

    ought(one->paste());

    same(whole(*one), std::string{"ala ma kota ala"});
}

TRIAL("Editor: kopiowanie bez zaznaczenia niczego nie rusza") {
    Sheet sheet;

    const auto one = shown(sheet, "abc");

    aside([one] { ought(!one->copy()); });
}

// Wyciecie idzie przez to samo kasowanie zaznaczenia, co Backspace, wiec jest jedna zmiana
// w dzienniku i wraca jednym cofnieciem.
TRIAL("Editor: wycięcie zabiera tekst i cofa się jednym ruchem") {
    Sheet sheet;

    const auto one = shown(sheet, "ala ma kota");

    one->select(4, 6);

    aside([one] { ought(one->cut()); });

    same(whole(*one), std::string{"ala  kota"});

    one->undo();

    same(whole(*one), std::string{"ala ma kota"});
    ought(!one->undoable());
}

TRIAL("Editor: wklejenie na zaznaczeniu zastępuje je") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    one->select(0, 2);

    aside([one] { ought(one->copy()); });

    one->select(4, 6);

    ought(one->paste());

    same(whole(*one), std::string{"abcdab"});
}

TRIAL("Editor: tylko do odczytu kopiuje, ale nie wycina i nie wkleja") {
    Sheet sheet;

    const auto one = shown(sheet, "ala ma kota");

    one->select(0, 3);

    aside([one] { ought(one->copy()); });

    one->readonly(true);

    ought(!one->cut());
    ought(!one->paste());

    same(whole(*one), std::string{"ala ma kota"});
    ought(!one->dirty());
}

// Oba komplety klawiszy robia to samo: Ctrl z litera z reszty swiata, Insert z Deletem
// z Midnight Commandera.
// **Ctrl z literą nie jest sprawą edytora.** Stał tu drugi komplet klawiszy schowka obok tego
// z Midnight Commandera, i to on był kłopotem: w trybie surowym Ctrl+C nie jest przerwaniem, tylko
// zwykłym klawiszem, więc edytor połykał go, ilekroć miał ognisko — a aplikacja, która chciała go
// na wyjście, przestawała go widzieć i nie mogła tego odwołać.
TRIAL("Editor: ctrl z literą przechodzi wyżej, schowka nie rusza") {
    Sheet sheet;

    const auto one = shown(sheet, "ala ma kota");

    one->focus();
    one->select(0, 4);

    ui::KeyEvent copy{ui::Key{U'c', ui::Mods{true, false, false}}};

    ought(!sheet.deal(copy));

    one->cursor(11);

    ui::KeyEvent paste{ui::Key{U'v', ui::Mods{true, false, false}}};

    ought(!sheet.deal(paste));

    ui::KeyEvent cut{ui::Key{U'x', ui::Mods{true, false, false}}};

    ought(!sheet.deal(cut));

    same(whole(*one), std::string{"ala ma kota"});
}

// Kto woli odruch z reszty świata, dokłada go uchwytem — i wtedy działa, bo uchwyt widzi klawisz
// niezależnie od tego, co robi `react`.
TRIAL("Editor: schowek pod ctrl da się dołożyć uchwytem") {
    Sheet sheet;

    const auto one = shown(sheet, "ala ma kota");

    one->focus();
    one->select(0, 4);

    const std::weak_ptr<ui::Editor> weak = one;

    (void)one->on<ui::KeyEvent>([weak](ui::KeyEvent& what) {
        if (!what.mods().ctrl || what.key().letter() != U'c') return false;

        const auto mine = weak.lock();

        return mine && mine->copy();
    });

    ui::KeyEvent copy{ui::Key{U'c', ui::Mods{true, false, false}}};

    aside([&sheet, &copy] { ought(sheet.deal(copy)); });

    same(one->selection(), std::string{"ala "});
}

TRIAL("Editor: insert i delete z modyfikatorami obsługują schowek") {
    Sheet sheet;

    const auto one = shown(sheet, "ala ma kota");

    one->focus();
    one->select(0, 4);

    ui::KeyEvent copy{ui::Key{ui::Code::insert, ui::Mods{true, false, false}}};

    aside([&sheet, &copy] { ought(sheet.deal(copy)); });

    one->cursor(11);

    ui::KeyEvent paste{ui::Key{ui::Code::insert, ui::Mods{false, false, true}}};

    ought(sheet.deal(paste));
    same(whole(*one), std::string{"ala ma kotaala "});

    one->select(0, 4);

    ui::KeyEvent cut{ui::Key{ui::Code::remove, ui::Mods{false, false, true}}};

    aside([&sheet, &cut] { ought(sheet.deal(cut)); });

    same(whole(*one), std::string{"ma kotaala "});
}

// Sam Delete bez shiftu ma dalej kasowac znak, a nie wycinac.
TRIAL("Editor: sam delete kasuje znak, a nie wycina") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    one->focus();
    one->cursor(0);

    ui::KeyEvent remove{ui::Code::remove};

    ought(sheet.deal(remove));
    same(whole(*one), std::string{"bcdef"});
}

// --- Mysz ----------------------------------------------------------------------------------------

TRIAL("Editor: dwuklik bierze wyraz") {
    Sheet sheet;

    const auto one = shown(sheet, "ala ma kota\ndrugi");

    (void)sheet.render({{0, 0}, {20, 3}});

    ui::DoubleClickEvent what{{5, 0}, ui::Mouse::left};

    ought(sheet.deal(what));
    same(one->selection(), std::string{"ma"});
    same(one->anchor(), std::size_t{4});
    same(one->cursor(), std::size_t{6});
}

// Ogonek jest literą tą samą regułą, po której grzebień szuka całych wyrazów — inaczej dwuklik
// i szukanie zaczęłyby się różnić.
TRIAL("Editor: dwuklik nie rozcina wyrazu z ogonkiem") {
    Sheet sheet;

    const auto one = shown(sheet, "ma k\xc4\x85t tam");

    (void)sheet.render({{0, 0}, {20, 2}});

    ui::DoubleClickEvent what{{3, 0}, ui::Mouse::left};

    ought(sheet.deal(what));
    same(one->selection(), std::string{"k\xc4\x85t"});
}

// Klik w miejsce, ktore wyrazem nie jest, bierze ciag takich samych — dwuklik nigdy nie wyglada
// na zignorowany.
TRIAL("Editor: dwuklik poza wyrazem bierze ciąg znaków tego samego rodzaju") {
    Sheet sheet;

    const auto one = shown(sheet, "ala   ma");

    (void)sheet.render({{0, 0}, {20, 2}});

    ui::DoubleClickEvent what{{4, 0}, ui::Mouse::left};

    ought(sheet.deal(what));
    same(one->selection(), std::string{"   "});
}

TRIAL("Editor: dwuklik za tabulatorem bierze wyraz po nim") {
    Sheet sheet;

    const auto one = shown(sheet, "\tala ma");

    (void)sheet.render({{0, 0}, {20, 2}});

    ui::DoubleClickEvent what{{9, 0}, ui::Mouse::left};

    ought(sheet.deal(what));
    same(one->selection(), std::string{"ala"});
}

// Wcisniecie stawia kursor razem z kotwica, ruch z wcisnietym przyciskiem rozciaga zaznaczenie.
TRIAL("Editor: ciągnięcie myszą rozciąga zaznaczenie") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef\nghijkl");

    (void)sheet.render({{0, 0}, {20, 3}});

    ui::MouseEvent down{{2, 0}, ui::Press::down, ui::Mouse::left};

    ought(sheet.deal(down));
    same(one->cursor(), std::size_t{2});
    ought(!one->selected());

    ui::MouseEvent move{{5, 0}, ui::Press::move, ui::Mouse::left};

    ought(sheet.deal(move));
    same(one->anchor(), std::size_t{2});
    same(one->selection(), std::string{"cde"});

    ui::MouseEvent far{{3, 1}, ui::Press::move, ui::Mouse::left};

    ought(sheet.deal(far));
    same(one->selection(), std::string{"cdef\nghi"});

    ui::MouseEvent up{{3, 1}, ui::Press::up, ui::Mouse::left};

    ought(sheet.deal(up));

    // Po puszczeniu ruch nad edytorem nie ma juz prawa niczego rozciagac.
    ui::MouseEvent after{{5, 1}, ui::Press::move, ui::Mouse::none};

    ought(sheet.deal(after));
    same(one->selection(), std::string{"cdef\nghi"});
}

// Skrajny rzad przewija o wiersz, a szybkosc bierze sie z tego, ile uzytkownik naprawde rusza
// mysza — kiedy stanie, przewijanie staje razem z nim. Zegara do tego nie trzeba.
TRIAL("Editor: ciągnięcie w skrajnym rzędzie przewija") {
    Sheet sheet;

    const auto one = shown(sheet, many(20));

    (void)sheet.render({{0, 0}, {20, 3}});

    same(*one->first(), std::size_t{0});

    ui::MouseEvent down{{1, 0}, ui::Press::down, ui::Mouse::left};

    ought(sheet.deal(down));

    ui::MouseEvent low{{1, 2}, ui::Press::move, ui::Mouse::left};

    ought(sheet.deal(low));
    (void)sheet.render({{0, 0}, {20, 3}});
    same(*one->first(), std::size_t{1});

    ought(sheet.deal(low));
    (void)sheet.render({{0, 0}, {20, 3}});
    same(*one->first(), std::size_t{2});

    ought(one->selected());

    ui::MouseEvent high{{1, 0}, ui::Press::move, ui::Mouse::left};

    ought(sheet.deal(high));
    (void)sheet.render({{0, 0}, {20, 3}});
    same(*one->first(), std::size_t{1});
}

// --- Szukanie ------------------------------------------------------------------------------------

TRIAL("Editor: szukanie znajduje i zaznacza") {
    Sheet sheet;

    const auto one = shown(sheet, "pierwszy\ndrugi\ntrzeci drugi");

    const Comb how{"drugi", {}};

    const std::optional<Hit> got = one->find(how, 0, false);

    ought(got.has_value());
    same(got->from, std::size_t{9});
    same(got->upto, std::size_t{14});
    same(one->selection(), std::string{"drugi"});
    same(one->line(), std::size_t{1});
}

TRIAL("Editor: szukanie idzie dalej od podanego miejsca") {
    Sheet sheet;

    const auto one = shown(sheet, "pierwszy\ndrugi\ntrzeci drugi");

    const Comb how{"drugi", {}};

    same(one->find(how, 10, false)->from, std::size_t{22});
    ought(!one->find(how, 23, false).has_value());
}

TRIAL("Editor: szukanie wstecz bierze poprzednie dopasowanie") {
    Sheet sheet;

    const auto one = shown(sheet, "ala\nma\nala\nkota\nala");

    const Comb how{"ala", {}};

    same(one->find(how, 19, true)->from, std::size_t{16});
    same(one->find(how, 16, true)->from, std::size_t{7});
    same(one->find(how, 7, true)->from, std::size_t{0});
    ought(!one->find(how, 0, true).has_value());
}

TRIAL("Editor: szukanie nie znajduje, czego nie ma") {
    Sheet sheet;

    const auto one = shown(sheet, "abc\ndef");

    const Comb how{"xyz", {}};

    ought(!one->find(how, 0, false).has_value());
    ought(!one->find(how, 7, true).has_value());
}

// Zly wzorzec nie ma prawa niczego zrobic — ani znalezc, ani rzucic.
TRIAL("Editor: niegotowy grzebień nic nie znajduje") {
    Sheet sheet;

    const auto one = shown(sheet, "abc");

    const Comb how{"[", Sift{true, false, false}};

    ought(!one->find(how, 0, false).has_value());
}

TRIAL("Editor: szukanie wyrażeniem chodzi po wierszach") {
    Sheet sheet;

    const auto one = shown(sheet, "rok 2026\nbez liczb\nmiesiac 12");

    const Comb how{"[0-9]+", Sift{true, false, false}};

    same(one->selection(), std::string{});

    ought(one->find(how, 0, false).has_value());
    same(one->selection(), std::string{"2026"});

    ought(one->find(how, 8, false).has_value());
    same(one->selection(), std::string{"12"});
}

// Wzorzec nie przekracza konca wiersza — szukamy wierszami, bo wierszami mowi zwoj.
TRIAL("Editor: wzorzec nie przechodzi przez koniec wiersza") {
    Sheet sheet;

    const auto one = shown(sheet, "ab\ncd");

    const Comb how{"b\nc", {}};

    ought(!one->find(how, 0, false).has_value());
}

// Szukanie w dokumencie dluzszym niz jedna porcja wierszy — porcje licza bajt poczatku same,
// wiec pomylka o jeden wyszlaby dopiero za granica porcji.
TRIAL("Editor: szukanie przechodzi przez granicę porcji") {
    Sheet sheet;

    std::string big;

    for (int which = 0; which < 300; ++which) big += "wiersz\n";

    big += "igla";

    const auto one = shown(sheet, big);

    const Comb how{"igla", {}};

    const std::optional<Hit> got = one->find(how, 0, false);

    ought(got.has_value());
    same(got->from, big.size() - 4);
    same(one->line(), std::size_t{300});

    same(one->find(how, big.size(), true)->from, big.size() - 4);
}

// --- Zamiana -------------------------------------------------------------------------------------

TRIAL("Editor: zamiana bez zaznaczenia najpierw pokazuje dopasowanie") {
    Sheet sheet;

    const auto one = shown(sheet, "ala ma ala");

    const Comb how{"ala", {}};

    same(one->replace(how, "ola", false), std::size_t{0});
    same(one->selection(), std::string{"ala"});
    same(whole(*one), std::string{"ala ma ala"});
}

TRIAL("Editor: zamiana bierze zaznaczone dopasowanie i pokazuje następne") {
    Sheet sheet;

    const auto one = shown(sheet, "ala ma ala");

    const Comb how{"ala", {}};

    (void)one->find(how, 0, false);

    // Zamiana zostawia zaznaczone **nastepne** dopasowanie, wiec drugie wolanie zamienia je bez
    // szukania. Pomocnicze `whole` po drodze by to zepsulo — ono zaznacza caly dokument.
    same(one->replace(how, "ola", false), std::size_t{1});
    same(one->selection(), std::string{"ala"});

    same(one->replace(how, "ola", false), std::size_t{1});
    same(one->selection(), std::string{});

    same(whole(*one), std::string{"ola ma ola"});
}

TRIAL("Editor: zamiana nie rusza zaznaczenia, które nie jest dopasowaniem") {
    Sheet sheet;

    const auto one = shown(sheet, "ala ma kota");

    const Comb how{"ala", {}};

    one->select(4, 6);

    same(one->replace(how, "ola", false), std::size_t{0});
    same(whole(*one), std::string{"ala ma kota"});
}

TRIAL("Editor: zamiana wszystkich bierze każde dopasowanie") {
    Sheet sheet;

    const auto one = shown(sheet, "ala ma ala\nala kot");

    const Comb how{"ala", {}};

    same(one->replace(how, "ola", true), std::size_t{3});
    same(whole(*one), std::string{"ola ma ola\nola kot"});
}

// Wszystkie zamiany sa przywiazane do pierwszej, wiec cofaja sie jednym ruchem.
TRIAL("Editor: zamiana wszystkich cofa się jednym ruchem") {
    Sheet sheet;

    const auto one = shown(sheet, "ala ma ala\nala kot");

    const Comb how{"ala", {}};

    same(one->replace(how, "ola", true), std::size_t{3});

    one->undo();

    same(whole(*one), std::string{"ala ma ala\nala kot"});
    ought(!one->undoable());

    one->redo();

    same(whole(*one), std::string{"ola ma ola\nola kot"});
}

// Wstawka niosaca w sobie wzorzec nie ma prawa zapetlic zamiany.
TRIAL("Editor: zamiana na tekst ze wzorcem w środku się nie zapętla") {
    Sheet sheet;

    const auto one = shown(sheet, "ala ala");

    const Comb how{"ala", {}};

    same(one->replace(how, "alala", true), std::size_t{2});
    same(whole(*one), std::string{"alala alala"});
}

TRIAL("Editor: zamiana na nic kasuje dopasowania") {
    Sheet sheet;

    const auto one = shown(sheet, "a1b22c333d");

    const Comb how{"[0-9]+", Sift{true, false, false}};

    same(one->replace(how, "", true), std::size_t{3});
    same(whole(*one), std::string{"abcd"});
}

TRIAL("Editor: zamiana po całych wyrazach") {
    Sheet sheet;

    const auto one = shown(sheet, "ala alama ala");

    const Comb how{"ala", Sift{false, false, true}};

    same(one->replace(how, "ola", true), std::size_t{2});
    same(whole(*one), std::string{"ola alama ola"});
}

TRIAL("Editor: tylko do odczytu nie zamienia") {
    Sheet sheet;

    const auto one = shown(sheet, "ala ma ala");

    one->readonly(true);

    const Comb how{"ala", {}};

    same(one->replace(how, "ola", true), std::size_t{0});
    same(whole(*one), std::string{"ala ma ala"});
}

// --- Zapis ---------------------------------------------------------------------------------------

namespace {

// Ujście zbierające wszystko do napisu, z licznikiem kawałków — bo dokument większy od pamięci ma
// przepłynąć kawałkami, a nie jednym kęsem.
Drain into(std::string& what, int& much) {
    return [&what, &much](Bytes got) {
        for (const std::byte one : got) what.push_back(static_cast<char>(one));

        ++much;

        return true;
    };
}

}

TRIAL("Editor: zapis oddaje całą treść i czyści dokument") {
    Sheet sheet;

    const auto one = shown(sheet, "abc\ndef");

    one->cursor(0);
    one->insert("X");

    ought(one->dirty());

    std::string got;
    int much = 0;

    ought(one->save(into(got, much)));

    same(got, std::string{"Xabc\ndef"});
    same(much, 1);
    ought(!one->dirty());
}

TRIAL("Editor: zapis pustego dokumentu udaje się i nic nie oddaje") {
    Sheet sheet;

    const auto one = shown(sheet, "");

    std::string got;
    int much = 0;

    ought(one->save(into(got, much)));

    same(got, std::string{});
    same(much, 0);
}

// Niedokończony zapis nie jest zapisem: dokument zostaje brudny, żeby aplikacja nie uznała go
// za odłożony na dysk.
TRIAL("Editor: ujście, które odmówi, przerywa zapis") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    one->cursor(0);
    one->insert("X");

    ought(!one->save([](Bytes) { return false; }));
    ought(one->dirty());

    ought(!one->save({}));
}

TRIAL("Editor: duży dokument płynie kawałkami") {
    Sheet sheet;

    std::string big;

    for (int which = 0; which < 20000; ++which) big += "wiersz numer " + std::to_string(which) + "\n";

    const auto one = shown(sheet, big);

    std::string got;
    int much = 0;

    ought(one->save(into(got, much)));

    same(got, big);
    ought(much > 1);
}

// --- Odcięcie od źródła --------------------------------------------------------------------------

// Po zszyciu plik, z którego czytało źródło, wolno nadpisać. Sprawdzamy to najostrzej, jak się da:
// **kasujemy źródło** i pytamy dokument o treść.
TRIAL("Editor: po zszyciu dokument nie sięga już do źródła") {
    Sheet sheet;

    const std::string was = "pierwszy\ndrugi\ntrzeci";

    const auto raw = std::make_shared<Bytes>(lump(was));

    const auto one = std::make_shared<ui::Editor>();

    one->hold(
        [raw](std::size_t at, std::size_t most) {
            Bytes made;

            for (std::size_t which = at; which < raw->size() && which < at + most; ++which)
                made.push_back((*raw)[which]);

            return made;
        },
        was.size());

    sheet.children().add(one);

    one->cursor(9);
    one->insert("nowy ");

    one->settle();

    raw->clear();

    same(whole(*one), std::string{"pierwszy\nnowy drugi\ntrzeci"});
}

TRIAL("Editor: zszycie nie psuje dziennika") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    one->cursor(3);
    one->insert("XY");

    one->settle();

    ought(one->undoable());

    one->undo();

    same(whole(*one), std::string{"abcdef"});

    one->redo();

    same(whole(*one), std::string{"abcXYdef"});
}

// --- Kliknięcie ----------------------------------------------------------------------------------

TRIAL("Editor: kliknięcie stawia kursor w klikniętej kratce") {
    Sheet sheet;

    const auto one = shown(sheet, "abc\ndef");

    (void)sheet.render({{0, 0}, {20, 3}});

    ui::ClickEvent what{{2, 1}, ui::Mouse::left};

    ought(sheet.deal(what));
    same(one->cursor(), std::size_t{6});
}

// Rząd ekranu **nie jest** numerem wiersza: przy zawijaniu jeden wiersz zajmuje ich kilka.
// Liczone dodawaniem rzędu do górnego wiersza kliknięcie w drugą połowę zawiniętego wiersza
// lądowało w wierszu następnym.
TRIAL("Editor: kliknięcie w zawinięty wiersz trafia we właściwy wiersz") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdefgh\nxy");

    (void)sheet.render({{0, 0}, {4, 3}});

    ui::ClickEvent what{{1, 1}, ui::Mouse::left};

    ought(sheet.deal(what));
    same(one->cursor(), std::size_t{5});
    same(one->line(), std::size_t{0});
}

// Kolumna nie jest bajtem: tabulator zajmuje osiem kratek, a wszystkie wskazują ten sam bajt.
TRIAL("Editor: kliknięcie za tabulatorem trafia we właściwy bajt") {
    Sheet sheet;

    const auto one = shown(sheet, "\tab");

    (void)sheet.render({{0, 0}, {20, 2}});

    ui::ClickEvent what{{8, 0}, ui::Mouse::left};

    ought(sheet.deal(what));
    same(one->cursor(), std::size_t{1});

    ui::ClickEvent far{{3, 0}, ui::Mouse::left};

    ought(sheet.deal(far));
    same(one->cursor(), std::size_t{0});
}

TRIAL("Editor: kliknięcie za końcem wiersza staje na jego końcu") {
    Sheet sheet;

    const auto one = shown(sheet, "abc\ndef");

    (void)sheet.render({{0, 0}, {20, 3}});

    ui::ClickEvent what{{15, 0}, ui::Mouse::left};

    ought(sheet.deal(what));
    same(one->cursor(), std::size_t{3});
}

// --- Wiersz i kolumna ----------------------------------------------------------------------------

TRIAL("Editor: miejsce kursora liczy się wierszem i kratką") {
    Sheet sheet;

    const auto one = shown(sheet, "abc\ndef");

    one->cursor(5);

    same(one->line(), std::size_t{1});
    same(one->column(), std::size_t{1});
}

TRIAL("Editor: kolumna liczy tabulator jak ekran, a nie jak bajt") {
    Sheet sheet;

    const auto one = shown(sheet, "\tab");

    one->cursor(1);

    same(one->column(), std::size_t{8});

    one->cursor(2);

    same(one->column(), std::size_t{9});
}

// --- Zaznaczanie klawiszami ----------------------------------------------------------------------

TRIAL("Editor: kursor gasi zaznaczenie, a stretch je ciągnie") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    one->cursor(2);

    ought(!one->selected());

    one->stretch(5);

    ought(one->selected());
    same(one->anchor(), std::size_t{2});
    same(one->cursor(), std::size_t{5});
    same(one->selection(), std::string{"cde"});

    one->cursor(4);

    ought(!one->selected());
}

TRIAL("Editor: shift ze strzałką rozszerza zaznaczenie") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    one->focus();
    one->cursor(2);

    ui::KeyEvent right{ui::Key{ui::Code::right, ui::Mods{false, false, true}}};

    ought(sheet.deal(right));
    ought(sheet.deal(right));

    same(one->anchor(), std::size_t{2});
    same(one->cursor(), std::size_t{4});
    same(one->selection(), std::string{"cd"});
}

TRIAL("Editor: shift z ruchem w dół bierze zaznaczenie przez wiersze") {
    Sheet sheet;

    const auto one = shown(sheet, "abc\ndef");

    one->focus();

    (void)sheet.render({{0, 0}, {20, 3}});

    one->cursor(1);

    ui::KeyEvent down{ui::Key{ui::Code::down, ui::Mods{false, false, true}}};

    ought(sheet.deal(down));
    same(one->selection(), std::string{"bc\nd"});
}

TRIAL("Editor: shift z Home bierze początek wiersza") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    one->focus();
    one->cursor(4);

    ui::KeyEvent home{ui::Key{ui::Code::home, ui::Mods{false, false, true}}};

    ought(sheet.deal(home));
    same(one->selection(), std::string{"abcd"});
    same(one->anchor(), std::size_t{4});
}

// --- Stronicowanie -------------------------------------------------------------------------------

TRIAL("Editor: klawisz strony przenosi kursor o ekran") {
    Sheet sheet;

    const auto one = shown(sheet, many(10));

    one->focus();

    (void)sheet.render({{0, 0}, {20, 3}});

    ui::KeyEvent down{ui::Code::pagedown};

    ought(sheet.deal(down));
    same(one->line(), std::size_t{3});

    ought(sheet.deal(down));
    same(one->line(), std::size_t{6});

    ui::KeyEvent up{ui::Code::pageup};

    ought(sheet.deal(up));
    same(one->line(), std::size_t{3});
}

// Pod koniec dokumentu strona ma dojść do ostatniego wiersza, a nie odmówić ruchu.
TRIAL("Editor: strona pod koniec dokumentu staje na ostatnim wierszu") {
    Sheet sheet;

    const auto one = shown(sheet, many(10));

    one->focus();

    (void)sheet.render({{0, 0}, {20, 3}});

    one->cursor(24);

    same(one->line(), std::size_t{8});

    ui::KeyEvent down{ui::Code::pagedown};

    ought(sheet.deal(down));
    same(one->line(), std::size_t{9});

    ought(!sheet.deal(down));
    same(one->line(), std::size_t{9});
}

// Klawisz, którego edytor nie użył, ma iść wyżej — zjadanie go blokuje przodka.
TRIAL("Editor: strzałka bez dokąd oddaje klawisz wyżej") {
    Sheet sheet;

    const auto one = shown(sheet, "abc\ndef");

    one->focus();

    (void)sheet.render({{0, 0}, {20, 3}});

    one->cursor(1);

    ui::KeyEvent up{ui::Code::up};

    ought(!sheet.deal(up));

    one->cursor(5);

    ui::KeyEvent down{ui::Code::down};

    ought(!sheet.deal(down));
}

TRIAL("Editor: F-klawisze przechodzą wyżej") {
    Sheet sheet;

    const auto one = shown(sheet, "abc");

    one->focus();

    ui::KeyEvent f2{ui::Code::f2};

    ought(!sheet.deal(f2));
}

// --- Po zmianie ----------------------------------------------------------------------------------

// Szpula czyta bajty przez tablicę kawałków, więc po zmianie **widzi nową treść** — a jej kotwice
// za miejscem zmiany idą do kosza, żeby nie wskazywały w stare miejsca.
TRIAL("Editor: ekran pokazuje treść po zmianie") {
    Sheet sheet;

    const auto one = shown(sheet, "pierwszy\ndrugi");

    (void)sheet.render({{0, 0}, {20, 3}});

    one->cursor(0);
    one->insert("nowy ");

    const View made = sheet.render({{0, 0}, {20, 3}});

    same(said(made, 0), std::string{"nowy pierwszy"});
    same(said(made, 1), std::string{"drugi"});
}

TRIAL("Editor: wiele zmian w różnych miejscach nie gubi treści") {
    Sheet sheet;

    const auto one = shown(sheet, "aaa\nbbb\nccc\nddd");

    one->cursor(4);
    one->insert("X");

    one->cursor(0);
    one->insert("Y");

    one->cursor(*one->whole());
    one->insert("Z");

    same(whole(*one), std::string{"Yaaa\nXbbb\nccc\ndddZ"});
    same(*one->lines(), std::size_t{4});
}
