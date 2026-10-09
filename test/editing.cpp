#include "proving.hpp"

#include <chrono>
#include <exception>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "faking.hpp"
#include "talcite/bytes.hpp"
#include "talcite/editor.hpp"
#include "talcite/encoding.hpp"
#include "talcite/feed.hpp"
#include "talcite/loop.hpp"
#include "talcite/promise.hpp"
#include "talcite/root.hpp"

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


std::string many(int lines) {
    std::string made;

    for (int one = 0; one < lines; ++one) made += "l" + std::to_string(one) + "\n";

    made.pop_back();

    return made;
}

// Edytor na atrapie schowka, żeby żadna próba nie sięgała do systemowego — na Windows dawniej
// nadpisywała użytkownikowi to, co miał skopiowane.
std::shared_ptr<ui::Editor> shown(Sheet& sheet, const std::string& what,
                                  const std::shared_ptr<faking::Clipboard>& kept = faking::clipboard()) {
    const auto one = std::make_shared<ui::Editor>(Seam<Clipboard>{kept});

    one->hold(bytes(what));

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
    same(one->text(), std::string{"a\nbb\nccc"});
}

// --- Zmiany --------------------------------------------------------------------------------------

TRIAL("Editor: wpisanie znaku przesuwa kursor i brudzi dokument") {
    Sheet sheet;

    const auto one = shown(sheet, "abc");

    one->cursor(1);
    one->insert("X");

    same(one->text(), std::string{"aXbc"});
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
    same(one->text(), std::string{"abc\ndef"});
}

TRIAL("Editor: kasowanie wstecz zabiera znak przed kursorem") {
    Sheet sheet;

    const auto one = shown(sheet, "abcd");

    one->cursor(3);
    one->back_erase(1);

    same(one->text(), std::string{"abd"});
    same(one->cursor(), std::size_t{2});
}

TRIAL("Editor: kasowanie w przód zabiera znak za kursorem") {
    Sheet sheet;

    const auto one = shown(sheet, "abcd");

    one->cursor(1);
    one->front_erase(1);

    same(one->text(), std::string{"acd"});
    same(one->cursor(), std::size_t{1});
}

TRIAL("Editor: skasowanie entera scala wiersze") {
    Sheet sheet;

    const auto one = shown(sheet, "abc\ndef");

    same(*one->lines(), std::size_t{2});

    one->cursor(4);
    one->back_erase(1);

    same(*one->lines(), std::size_t{1});
    same(one->text(), std::string{"abcdef"});
}

TRIAL("Editor: tylko do odczytu nie przyjmuje zmian") {
    Sheet sheet;

    const auto one = shown(sheet, "abc");

    one->readonly(true);
    one->insert("X");
    one->back_erase(1);

    same(one->text(), std::string{"abc"});
    ought(!one->dirty());
}

// --- Zaznaczenie ---------------------------------------------------------------------------------

TRIAL("Editor: zaznaczenie oddaje swój tekst") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    ought(one->selection().empty());

    one->selection().set(2, 5);

    ought(!one->selection().empty());
    same(one->selection().text(), std::string{"cde"});
    same(one->selection().anchor(), std::size_t{2});
    same(one->cursor(), std::size_t{5});
}

TRIAL("Editor: wpisanie na zaznaczeniu zastępuje je") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    one->selection().set(2, 5);
    one->insert("X");

    same(one->text(), std::string{"abXf"});
    ought(one->selection().empty());
}

TRIAL("Editor: kasowanie na zaznaczeniu zabiera całe") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    one->selection().set(1, 4);
    one->back_erase(1);

    same(one->text(), std::string{"aef"});
    same(one->cursor(), std::size_t{1});
}

// **Zaznaczenie to podobiekt**: kotwica i kursor pod jednym dachem. Parametry `set` nazywają się
// tym, czym są, więc zaznaczenie wstecz to po prostu kotwica za kursorem — a `from` i `upto`
// i tak oddają je po kolei.
TRIAL("Selection: wstecz, rozciągnięcie i zgaszenie") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    one->selection().set(5, 2);

    same(one->selection().anchor(), std::size_t{5});
    same(one->cursor(), std::size_t{2});
    same(one->selection().from(), std::size_t{2});
    same(one->selection().upto(), std::size_t{5});
    same(one->selection().text(), std::string{"cde"});

    one->selection().extend(0);

    same(one->selection().anchor(), std::size_t{5});
    same(one->cursor(), std::size_t{0});
    same(one->selection().text(), std::string{"abcde"});

    one->selection().clear();

    ought(one->selection().empty());
    same(one->cursor(), std::size_t{0});
    same(one->selection().anchor(), std::size_t{0});
    same(one->selection().text(), std::string{});

    // Poza dokumentem przycina, a kursor postawiony wprost gasi zaznaczenie.
    one->selection().set(99, 1);

    same(one->selection().anchor(), std::size_t{6});

    one->cursor(3);

    ought(one->selection().empty());
}

TRIAL("Selection: kasowanie zabiera zaznaczenie jedną zmianą, a puste nic nie robi") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    ought(!one->selection().erase());
    ought(!one->undoable());

    one->selection().set(4, 1);

    ought(one->selection().erase());
    same(one->text(), std::string{"aef"});
    same(one->cursor(), std::size_t{1});
    ought(one->selection().empty());

    one->undo();

    same(one->text(), std::string{"abcdef"});

    one->readonly(true);
    one->selection().set(0, 2);

    ought(!one->selection().erase());
    same(one->text(), std::string{"abcdef"});
}

// Delete przy zaznaczeniu kasuje zaznaczenie, a nie znaki za kursorem — tak samo jak Backspace.
TRIAL("Editor: kasowanie w przód na zaznaczeniu zabiera całe") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    one->selection().set(1, 4);
    one->front_erase(1);

    same(one->text(), std::string{"aef"});
    same(one->cursor(), std::size_t{1});
}

// **Cały dokument wprost**, bez ruszania kursora i zaznaczenia — dawniej wołający zaznaczał
// wszystko, czytał i przywracał.
TRIAL("Editor: text oddaje cały dokument i niczego nie rusza") {
    Sheet sheet;

    const auto empty = std::make_shared<ui::Editor>();

    same(empty->text(), std::string{});

    const auto one = shown(sheet, "ab\ncd");

    one->selection().set(1, 3);

    same(one->text(), std::string{"ab\ncd"});
    same(one->selection().anchor(), std::size_t{1});
    same(one->cursor(), std::size_t{3});
}

// Zaznaczenie rysuje się znacznikami, po jednym na wiersz — ale tylko dla tego, co widać.
TRIAL("Editor: zaznaczenie widać na ekranie") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    one->selection().set(2, 5);

    const View made = sheet.render({{0, 0}, {20, 2}});

    ought(!made.at({1, 0}).style.reverse);
    ought(made.at({2, 0}).style.reverse);
    ought(made.at({4, 0}).style.reverse);
    ought(!made.at({5, 0}).style.reverse);
}

TRIAL("Editor: zaznaczenie przez dwa wiersze daje znacznik na każdym") {
    Sheet sheet;

    const auto one = shown(sheet, "abc\ndef");

    one->selection().set(1, 6);

    const View made = sheet.render({{0, 0}, {20, 3}});

    ought(!made.at({0, 0}).style.reverse);
    ought(made.at({1, 0}).style.reverse);
    ought(made.at({2, 0}).style.reverse);
    ought(made.at({0, 1}).style.reverse);
    ought(made.at({1, 1}).style.reverse);
    ought(!made.at({2, 1}).style.reverse);
}

// --- Skok do wiersza ------------------------------------------------------------------------------

// `line(n)` stawia kursor na początku wiersza n (od zera) i gasi zaznaczenie; za końcem — na
// ostatnim wierszu; widok idzie za kursorem. Para do `line()`, więc to, co jedno oddaje, drugie
// przyjmuje.
TRIAL("Editor: line(n) skacze na początek wiersza, za końcem na ostatni, gasi zaznaczenie") {
    Sheet sheet;

    const auto one = shown(sheet, many(100));

    (void)sheet.render({{0, 0}, {20, 5}});

    one->selection().set(1, 4);
    one->line(42);

    same(one->line(), std::size_t{42});
    same(one->column(), std::size_t{0});
    ought(one->selection().empty());

    const View made = sheet.render({{0, 0}, {20, 5}});

    bool seen = false;

    for (int y = 0; y < 5; ++y)
        if (said(made, y) == "l42") seen = true;

    ought(seen);

    one->line(9999);

    same(one->line(), std::size_t{99});
    same(one->column(), std::size_t{0});

    one->line(0);

    same(one->cursor(), std::size_t{0});

    // Pusty dokument: zero i nic więcej.
    const auto empty = std::make_shared<ui::Editor>();

    empty->hold(bytes(""));
    empty->line(5);

    same(empty->cursor(), std::size_t{0});
}

// --- Blok mcedit ---------------------------------------------------------------------------------

// F3 zaczyna blok w kursorze, ruch rozciąga, drugie F3 kończy — i blok zostaje, choć kursor
// pójdzie dalej. Trzecie F3 zdejmuje stary i zaczyna nowy. Blok w tył porządkuje się min/max,
// zerowej długości znika.
TRIAL("Editor: mark zaczyna, kończy i zaczyna od nowa; blok trwa po ruchu kursora") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");
    ui::Editor::Block& block = one->block();

    ought(block.empty());
    ought(!block.pending());
    same(block.text(), std::string{});

    one->cursor(2);
    block.mark();

    ought(!block.empty());
    ought(block.pending());
    same(block.from(), std::size_t{2});
    same(block.upto(), std::size_t{2});

    // Rozciąganie: koniec jest kursorem — także klawiszem, bez Shift.
    one->focus();
    (void)sheet.render({{0, 0}, {20, 2}});

    ui::KeyEvent right{ui::Code::right};

    ought(sheet.dispatch(right));
    ought(sheet.dispatch(right));
    ought(sheet.dispatch(right));

    same(block.upto(), std::size_t{5});
    same(block.text(), std::string{"cde"});

    block.mark();

    ought(!block.empty());
    ought(!block.pending());

    one->cursor(0);

    same(block.from(), std::size_t{2});
    same(block.upto(), std::size_t{5});
    same(block.text(), std::string{"cde"});

    // Trzecie F3: nowy blok w kursorze, stary znika.
    block.mark();

    ought(block.pending());
    same(block.from(), std::size_t{0});

    // W tył: kursor przed początkiem, tekst i tak w porządku.
    block.clear();
    one->cursor(5);
    block.mark();
    one->cursor(2);

    same(block.text(), std::string{"cde"});

    block.mark();

    same(block.from(), std::size_t{2});
    same(block.upto(), std::size_t{5});

    // Zerowej długości: znika.
    block.mark();
    block.mark();

    ought(block.empty());
}

// F5: treść bloku wchodzi w kursorze, blok zostaje — a gdy leżał za kursorem, przesuwa się
// o swoją długość. Jedno cofnięcie, jedno ponowienie, blok wraca razem z tekstem.
TRIAL("Editor: copy wstawia blok przy kursorze i zostawia go, undo wraca jednym krokiem") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");
    ui::Editor::Block& block = one->block();

    ought(!block.copy());

    one->cursor(2);
    block.mark();
    one->cursor(5);
    block.mark();

    one->cursor(0);

    ought(block.copy());
    same(one->text(), std::string{"cdeabcdef"});
    same(one->cursor(), std::size_t{3});
    same(block.from(), std::size_t{5});
    same(block.upto(), std::size_t{8});
    same(block.text(), std::string{"cde"});

    one->undo();

    same(one->text(), std::string{"abcdef"});
    same(block.from(), std::size_t{2});
    same(block.upto(), std::size_t{5});
    ought(!one->undoable());

    one->redo();

    same(one->text(), std::string{"cdeabcdef"});
    same(block.from(), std::size_t{5});

    // Za blokiem: blok nie rusza się.
    one->undo();
    one->cursor(6);

    ought(block.copy());
    same(one->text(), std::string{"abcdefcde"});
    same(block.from(), std::size_t{2});
    same(block.upto(), std::size_t{5});

    // W trakcie rozciągania F5 najpierw kończy blok.
    one->undo();
    one->cursor(0);
    block.mark();
    one->cursor(2);

    ought(block.copy());
    ought(!block.pending());

    // Wpis skopiowania jest zapieczętowany: znak wpisany zaraz potem cofa się osobno. Między
    // skopiowaniem a znakiem nie może stać nic, co rusza kursorem — ruch kursora sam zamyka grupę
    // cofania i próba nie pytałaby wtedy o pieczęć. `text()` kursora nie rusza.
    one->focus();
    (void)sheet.render({{0, 0}, {20, 2}});

    ui::KeyEvent letter{U'X'};

    ought(sheet.dispatch(letter));
    same(one->text(), std::string{"ababXcdef"});

    one->undo();

    same(one->text(), std::string{"ababcdef"});
}

// F6: blok wychodzi i wchodzi tam, gdzie stał kursor po wycięciu — przed blokiem cel jest ten sam,
// za blokiem cofa się o długość. W bloku i na brzegu nic. Dwa wpisy dziennika związane w jeden.
TRIAL("Editor: move przenosi blok przed i za kursor, w bloku odmawia, undo jednym krokiem") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");
    ui::Editor::Block& block = one->block();

    ought(!block.move());

    one->cursor(2);
    block.mark();
    one->cursor(5);
    block.mark();

    // Przed blok.
    one->cursor(0);

    ought(block.move());
    same(one->text(), std::string{"cdeabf"});
    same(block.from(), std::size_t{0});
    same(block.upto(), std::size_t{3});
    same(one->cursor(), std::size_t{3});

    one->undo();

    same(one->text(), std::string{"abcdef"});
    same(block.from(), std::size_t{2});
    same(block.upto(), std::size_t{5});
    ought(!one->undoable());

    // Za blok: na koniec.
    one->cursor(6);

    ought(block.move());
    same(one->text(), std::string{"abfcde"});
    same(block.from(), std::size_t{3});
    same(block.upto(), std::size_t{6});
    same(one->cursor(), std::size_t{6});

    one->undo();

    same(one->text(), std::string{"abcdef"});

    // Ponowienie odtwarza i tekst, i blok.
    one->redo();

    same(one->text(), std::string{"abfcde"});
    same(block.from(), std::size_t{3});

    one->undo();

    // W bloku i na obu brzegach: nic.
    for (const std::size_t at : {std::size_t{2}, std::size_t{3}, std::size_t{5}}) {
        one->cursor(at);

        ought(!block.move());
        same(one->text(), std::string{"abcdef"});
        same(block.from(), std::size_t{2});
    }
}

// F8: blok znika z dokumentu i sam ze sobą. Kursor za blokiem cofa się, w bloku staje na jego
// początku, przed — zostaje. Cofnięcie przywraca tekst i blok.
TRIAL("Editor: erase usuwa blok i zdejmuje go, kursor według położenia, undo przywraca oba") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");
    ui::Editor::Block& block = one->block();

    ought(!block.erase());

    one->cursor(2);
    block.mark();
    one->cursor(5);
    block.mark();

    one->cursor(6);

    ought(block.erase());
    same(one->text(), std::string{"abf"});
    same(one->cursor(), std::size_t{3});
    ought(block.empty());

    one->undo();

    same(one->text(), std::string{"abcdef"});
    ought(!block.empty());
    same(block.from(), std::size_t{2});
    same(block.upto(), std::size_t{5});

    one->cursor(3);

    ought(block.erase());
    same(one->cursor(), std::size_t{2});

    one->undo();
    one->cursor(0);

    ought(block.erase());
    same(one->cursor(), std::size_t{0});
    same(one->text(), std::string{"abf"});
}

// Blok idzie za dokumentem: zmiana przed nim przesuwa, za nim nic, w środku wydłuża albo
// przycina, a wycięcie całego zdejmuje go.
TRIAL("Editor: blok przesuwa się i przycina za zmianami dokumentu") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");
    ui::Editor::Block& block = one->block();

    one->cursor(2);
    block.mark();
    one->cursor(5);
    block.mark();

    // Przed: przesunięcie.
    one->cursor(0);
    one->insert("XY");

    same(one->text(), std::string{"XYabcdef"});
    same(block.from(), std::size_t{4});
    same(block.upto(), std::size_t{7});
    same(block.text(), std::string{"cde"});

    // Za: nic.
    one->cursor(8);
    one->back_erase(1);

    same(one->text(), std::string{"XYabcde"});
    same(block.from(), std::size_t{4});
    same(block.upto(), std::size_t{7});

    // Dokładnie na początku: tekst idzie przed blok; dokładnie na końcu: nie wchodzi do bloku.
    one->cursor(4);
    one->insert("-");

    same(block.text(), std::string{"cde"});
    same(block.from(), std::size_t{5});

    one->cursor(8);
    one->insert("+");

    same(block.text(), std::string{"cde"});
    same(one->text(), std::string{"XYab-cde+"});

    // W środku: wydłuża.
    one->cursor(6);
    one->insert("Z");

    same(block.text(), std::string{"cZde"});

    // Nachodząca na początek: przycina do tego, co zostało.
    one->selection().set(4, 7);
    one->selection().erase();

    same(one->text(), std::string{"XYabde+"});
    same(block.text(), std::string{"de"});
    same(block.from(), std::size_t{4});
    same(block.upto(), std::size_t{6});

    // Cały blok wycięty: znika.
    one->selection().set(3, 7);
    one->selection().erase();

    ought(block.empty());

    // Cofnięcie przywraca go.
    one->undo();

    ought(!block.empty());
    same(block.text(), std::string{"de"});

    // Zamiana nachodząca na początek stawia początek **za** wstawionym; nachodząca na koniec
    // przycina koniec **do** miejsca zmiany — wstawiony tekst nie wchodzi do bloku ani z jednej,
    // ani z drugiej strony.
    one->hold(bytes("abcdef"));
    one->cursor(2);
    block.mark();
    one->cursor(5);
    block.mark();

    one->selection().set(1, 4);
    one->insert("QQ");

    same(one->text(), std::string{"aQQef"});
    same(block.text(), std::string{"e"});
    same(block.from(), std::size_t{3});

    one->hold(bytes("abcdef"));
    one->cursor(2);
    block.mark();
    one->cursor(5);
    block.mark();

    one->selection().set(4, 6);
    one->insert("QQ");

    same(one->text(), std::string{"abcdQQ"});
    same(block.text(), std::string{"cd"});
    same(block.upto(), std::size_t{4});

    // W trakcie rozciągania początek też idzie za zmianą przed nim.
    one->hold(bytes("abcdef"));
    one->cursor(2);
    block.mark();
    one->cursor(0);
    one->insert("XY");

    ought(block.pending());
    same(block.from(), std::size_t{4});
    same(block.text(), std::string{"ab"});
}

TRIAL("Editor: blok widać na ekranie, także w trakcie rozciągania; readonly i hold") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");
    ui::Editor::Block& block = one->block();

    one->cursor(2);
    block.mark();
    one->cursor(5);
    block.mark();
    one->cursor(0);

    View made = sheet.render({{0, 0}, {20, 2}});

    ought(!made.at({1, 0}).style.reverse);
    ought(made.at({2, 0}).style.reverse);
    ought(made.at({4, 0}).style.reverse);
    ought(!made.at({5, 0}).style.reverse);

    // W trakcie: rośnie z kursorem.
    block.clear();

    made = sheet.render({{0, 0}, {20, 2}});

    ought(!made.at({2, 0}).style.reverse);

    one->cursor(1);
    block.mark();
    one->cursor(4);

    made = sheet.render({{0, 0}, {20, 2}});

    ought(!made.at({0, 0}).style.reverse);
    ought(made.at({1, 0}).style.reverse);
    ought(made.at({3, 0}).style.reverse);
    ought(!made.at({4, 0}).style.reverse);

    block.mark();

    // Tylko do odczytu: czynności odmawiają, blok można stawiać.
    one->readonly(true);

    ought(!block.copy());
    ought(!block.move());
    ought(!block.erase());
    ought(!block.empty());
    same(one->text(), std::string{"abcdef"});

    one->readonly(false);

    // Nowy dokument zdejmuje blok.
    one->hold(bytes("xyz"));

    ought(block.empty());
    ought(!block.pending());
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

    ought(sheet.dispatch(what));
    same(one->text(), std::string{"Xab"});
}

TRIAL("Editor: strzałki chodzą po znakach i po wierszach") {
    Sheet sheet;

    const auto one = shown(sheet, "abc\ndef");

    one->focus();

    (void)sheet.render({{0, 0}, {20, 3}});

    ui::KeyEvent right{ui::Code::right};

    ought(sheet.dispatch(right));
    same(one->cursor(), std::size_t{1});

    ui::KeyEvent down{ui::Code::down};

    ought(sheet.dispatch(down));
    same(one->cursor(), std::size_t{5});

    ui::KeyEvent left{ui::Code::left};

    ought(sheet.dispatch(left));
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

    ought(sheet.dispatch(back));
    same(one->text(), std::string{"ab"});
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

    same(one->text(), std::string{"abc"});
}

TRIAL("Editor: cofnięcie oddaje dokument sprzed zmiany, ponowienie go przywraca") {
    Sheet sheet;

    const auto one = shown(sheet, "abc");

    one->cursor(1);
    one->insert("X");

    same(one->text(), std::string{"aXbc"});
    ought(one->undoable());
    ought(!one->redoable());

    one->undo();

    same(one->text(), std::string{"abc"});
    ought(!one->undoable());
    ought(one->redoable());

    one->redo();

    same(one->text(), std::string{"aXbc"});
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

    same(one->text(), std::string{"abc"});

    one->undo();

    same(one->text(), std::string{});
    ought(!one->undoable());
}

TRIAL("Editor: spacja kończy wyraz, więc cofa się osobno") {
    Sheet sheet;

    const auto one = shown(sheet, "");

    for (const char sign : std::string{"ala "}) one->insert(std::string{sign});
    for (const char sign : std::string{"ma"}) one->insert(std::string{sign});

    same(one->text(), std::string{"ala ma"});

    one->undo();

    same(one->text(), std::string{"ala "});

    one->undo();

    same(one->text(), std::string{});
}

TRIAL("Editor: ruch kursora zamyka wyraz") {
    Sheet sheet;

    const auto one = shown(sheet, "xy");

    one->cursor(2);
    one->insert("a");
    one->insert("b");

    one->cursor(0);
    one->insert("Z");

    same(one->text(), std::string{"Zxyab"});

    one->undo();

    same(one->text(), std::string{"xyab"});

    one->undo();

    same(one->text(), std::string{"xy"});
}

TRIAL("Editor: enter jest osobnym krokiem") {
    Sheet sheet;

    const auto one = shown(sheet, "");

    one->insert("a");
    one->insert("\n");
    one->insert("b");

    same(*one->lines(), std::size_t{2});

    one->undo();

    same(one->text(), std::string{"a\n"});

    one->undo();

    same(one->text(), std::string{"a"});
    same(*one->lines(), std::size_t{1});
}

// Wpisanie na zaznaczeniu to **jedna** zmiana. Gdyby było kasowaniem i wpisaniem po nim, jedno
// cofnięcie oddałoby dokument bez zaznaczonego kawałka zamiast tego, co w nim stało.
TRIAL("Editor: wpisanie na zaznaczeniu cofa się jednym ruchem") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    one->selection().set(2, 5);
    one->insert("X");

    same(one->text(), std::string{"abXf"});

    one->undo();

    same(one->text(), std::string{"abcdef"});
    ought(!one->undoable());
}

TRIAL("Editor: trzymany backspace zbiera się w jeden krok") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    one->cursor(6);
    one->back_erase(1);
    one->back_erase(1);
    one->back_erase(1);

    same(one->text(), std::string{"abc"});

    one->undo();

    same(one->text(), std::string{"abcdef"});
    same(one->cursor(), std::size_t{6});
}

// **Długi trzymany Backspace.** Znikające znaki zbierają się od tyłu; dawniej każdy następny stawał
// przed poprzednimi, więc wpis przepisywał się w całości przy każdym znaku — przy dużym pliku
// kwadrat. Znaki wielobajtowe wracają przy cofnięciu całe.
TRIAL("Editor: długi trzymany backspace cofa się jednym ruchem w całości") {
    Sheet sheet;

    std::string was;

    for (int at = 0; at < 300; ++at) was += "zażółć gęślą ";

    const auto one = shown(sheet, was);

    one->cursor(was.size());

    while (one->cursor() > 0) one->back_erase(1);

    same(one->text(), std::string{});

    one->undo();

    same(one->text(), was);
    ought(!one->undoable());

    one->redo();

    same(one->text(), std::string{});
}

// Trzymany Backspace po kilka bajtów naraz — każdy kawałek wraca w swojej kolejności.
TRIAL("Editor: backspace po kilka bajtów wraca w kolejności") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdefghi");

    one->cursor(9);
    one->back_erase(3);
    one->back_erase(3);
    one->back_erase(3);

    same(one->text(), std::string{});

    one->undo();

    same(one->text(), std::string{"abcdefghi"});
}

// Trzymany Backspace, a za nim trzymany Delete w tym samym miejscu, to jeden wpis — i wraca w dobrej
// kolejności: najpierw to, co było przed kursorem, potem to, co za nim.
TRIAL("Editor: backspace i delete w tym samym miejscu wracają w kolejności") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    one->cursor(3);
    one->back_erase(1);
    one->back_erase(1);
    one->front_erase(1);
    one->front_erase(1);

    same(one->text(), std::string{"af"});

    one->undo();

    same(one->text(), std::string{"abcdef"});
    ought(!one->undoable());
}

TRIAL("Editor: trzymany delete zbiera się w jeden krok") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    one->cursor(1);
    one->front_erase(1);
    one->front_erase(1);

    same(one->text(), std::string{"adef"});

    one->undo();

    same(one->text(), std::string{"abcdef"});
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
    same(one->text(), std::string{"Yabc"});
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
    same(one->text(), std::string{"ab\ndef\nghi"});

    one->cursor(2);
    one->front_erase(1);

    same(*one->lines(), std::size_t{2});

    one->undo();

    same(*one->lines(), std::size_t{3});
    same(one->text(), std::string{"ab\ndef\nghi"});
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
    same(one->text(), std::string{"abc"});
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

    same(one->text(), std::string{"aXbc"});
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
            const std::string before = one->text();

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
            if (one->text() != before) steps.push_back(before);
        }

        while (!steps.empty()) {
            one->undo();

            same(one->text(), steps.back());

            steps.pop_back();
        }

        ought(!one->undoable());
        ought(!one->dirty());
        same(one->text(), was);
    }
}

// Skok po wyrazach chodzi ta sama regula, co szukanie calych wyrazow i dwuklik — jedna definicja
// wyrazu w calej bibliotece.
TRIAL("Editor: ctrl ze strzałką idzie po wyrazach") {
    Sheet sheet;

    const auto one = shown(sheet, "ala ma kota");

    one->focus();

    ui::KeyEvent right{ui::Key{ui::Code::right, ui::Mods{true, false, false}}};

    ought(sheet.dispatch(right));
    same(one->cursor(), std::size_t{4});

    ought(sheet.dispatch(right));
    same(one->cursor(), std::size_t{7});

    ought(sheet.dispatch(right));
    same(one->cursor(), std::size_t{11});

    ui::KeyEvent left{ui::Key{ui::Code::left, ui::Mods{true, false, false}}};

    ought(sheet.dispatch(left));
    same(one->cursor(), std::size_t{7});

    ought(sheet.dispatch(left));
    same(one->cursor(), std::size_t{4});

    ought(sheet.dispatch(left));
    same(one->cursor(), std::size_t{0});
}

TRIAL("Editor: skok po wyrazach przechodzi między wierszami") {
    Sheet sheet;

    const auto one = shown(sheet, "abc\ndef");

    one->focus();

    one->cursor(3);

    ui::KeyEvent right{ui::Key{ui::Code::right, ui::Mods{true, false, false}}};

    ought(sheet.dispatch(right));
    same(one->cursor(), std::size_t{4});

    ui::KeyEvent left{ui::Key{ui::Code::left, ui::Mods{true, false, false}}};

    ought(sheet.dispatch(left));
    same(one->cursor(), std::size_t{3});
}

TRIAL("Editor: skok po wyrazach nie rozcina ogonka") {
    Sheet sheet;

    const auto one = shown(sheet, "k\xc4\x85t ma");

    one->focus();

    ui::KeyEvent right{ui::Key{ui::Code::right, ui::Mods{true, false, false}}};

    ought(sheet.dispatch(right));
    same(one->cursor(), std::size_t{5});
}

TRIAL("Editor: ctrl ze strzałką i shiftem ciągnie zaznaczenie") {
    Sheet sheet;

    const auto one = shown(sheet, "ala ma kota");

    one->focus();

    ui::KeyEvent right{ui::Key{ui::Code::right, ui::Mods{true, false, true}}};

    ought(sheet.dispatch(right));
    ought(sheet.dispatch(right));

    same(one->selection().anchor(), std::size_t{0});
    same(one->selection().text(), std::string{"ala ma "});
}

TRIAL("Editor: skok po wyrazach na krańcach stoi w miejscu") {
    Sheet sheet;

    const auto one = shown(sheet, "ala");

    one->focus();

    ui::KeyEvent left{ui::Key{ui::Code::left, ui::Mods{true, false, false}}};

    ought(sheet.dispatch(left));
    same(one->cursor(), std::size_t{0});

    one->cursor(3);

    ui::KeyEvent right{ui::Key{ui::Code::right, ui::Mods{true, false, false}}};

    ought(sheet.dispatch(right));
    same(one->cursor(), std::size_t{3});
}

// --- Schowek -------------------------------------------------------------------------------------

TRIAL("Editor: kopiowanie i wklejanie chodzą przez schowek") {
    Sheet sheet;

    const auto one = shown(sheet, "ala ma kota");

    one->selection().set(0, 3);

    ought(one->copy());

    one->cursor(11);
    one->insert(" ");

    ought(one->paste());

    same(one->text(), std::string{"ala ma kota ala"});
}

TRIAL("Editor: kopiowanie bez zaznaczenia niczego nie rusza") {
    Sheet sheet;

    const auto one = shown(sheet, "abc");

    ought(!one->copy());
}

// Wyciecie idzie przez to samo kasowanie zaznaczenia, co Backspace, wiec jest jedna zmiana
// w dzienniku i wraca jednym cofnieciem.
TRIAL("Editor: wycięcie zabiera tekst i cofa się jednym ruchem") {
    Sheet sheet;

    const auto one = shown(sheet, "ala ma kota");

    one->selection().set(4, 6);

    ought(one->cut());

    same(one->text(), std::string{"ala  kota"});

    one->undo();

    same(one->text(), std::string{"ala ma kota"});
    ought(!one->undoable());
}

TRIAL("Editor: wklejenie na zaznaczeniu zastępuje je") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    one->selection().set(0, 2);

    ought(one->copy());

    one->selection().set(4, 6);

    ought(one->paste());

    same(one->text(), std::string{"abcdab"});
}

TRIAL("Editor: tylko do odczytu kopiuje, ale nie wycina i nie wkleja") {
    Sheet sheet;

    const auto one = shown(sheet, "ala ma kota");

    one->selection().set(0, 3);

    ought(one->copy());

    one->readonly(true);

    ought(!one->cut());
    ought(!one->paste());

    same(one->text(), std::string{"ala ma kota"});
    ought(!one->dirty());
}

TRIAL("Editor: kopia idzie do podanego schowka") {
    Sheet sheet;

    const auto kept = faking::clipboard();
    const auto one = shown(sheet, "ala ma kota", kept);

    one->selection().set(0, 3);

    ought(one->copy());
    same(kept->kept.value_or(""), std::string{"ala"});
}

// Gdy schowek odmówi, wycięcie nie kasuje: dokument zostałby bez tekstu, którego nie ma dokąd
// wkleić.
TRIAL("Editor: odmowa schowka nie wycina") {
    Sheet sheet;

    const auto kept = faking::clipboard();

    kept->refusing = true;

    const auto one = shown(sheet, "ala ma kota", kept);

    one->selection().set(0, 3);

    ought(!one->cut());
    same(kept->copies, 1);
    same(one->text(), std::string{"ala ma kota"});
    ought(!one->dirty());
}

TRIAL("Editor: dwa edytory na jednym schowku wymieniają tekst") {
    Sheet first;
    Sheet second;

    const auto kept = faking::clipboard();
    const auto one = shown(first, "ala ma kota", kept);
    const auto two = shown(second, "pies", kept);

    one->selection().set(4, 6);

    ought(one->cut());

    two->cursor(4);

    ought(two->paste());
    same(two->text(), std::string{"piesma"});
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
    one->selection().set(0, 4);

    ui::KeyEvent copy{ui::Key{U'c', ui::Mods{true, false, false}}};

    ought(!sheet.dispatch(copy));

    one->cursor(11);

    ui::KeyEvent paste{ui::Key{U'v', ui::Mods{true, false, false}}};

    ought(!sheet.dispatch(paste));

    ui::KeyEvent cut{ui::Key{U'x', ui::Mods{true, false, false}}};

    ought(!sheet.dispatch(cut));

    same(one->text(), std::string{"ala ma kota"});
}

// Kto woli odruch z reszty świata, dokłada go uchwytem — i wtedy działa, bo uchwyt widzi klawisz
// niezależnie od tego, co robi `handle`.
TRIAL("Editor: schowek pod ctrl da się dołożyć uchwytem") {
    Sheet sheet;

    const auto one = shown(sheet, "ala ma kota");

    one->focus();
    one->selection().set(0, 4);

    const std::weak_ptr<ui::Editor> weak = one;

    (void)one->on<ui::KeyEvent>([weak](ui::KeyEvent& what) {
        if (!what.mods().ctrl || what.key().letter() != U'c') return false;

        const auto mine = weak.lock();

        return mine && mine->copy();
    });

    ui::KeyEvent copy{ui::Key{U'c', ui::Mods{true, false, false}}};

    ought(sheet.dispatch(copy));

    same(one->selection().text(), std::string{"ala "});
}

TRIAL("Editor: insert i delete z modyfikatorami obsługują schowek") {
    Sheet sheet;

    const auto one = shown(sheet, "ala ma kota");

    one->focus();
    one->selection().set(0, 4);

    ui::KeyEvent copy{ui::Key{ui::Code::insert, ui::Mods{true, false, false}}};

    ought(sheet.dispatch(copy));

    one->cursor(11);

    ui::KeyEvent paste{ui::Key{ui::Code::insert, ui::Mods{false, false, true}}};

    ought(sheet.dispatch(paste));
    same(one->text(), std::string{"ala ma kotaala "});

    one->selection().set(0, 4);

    ui::KeyEvent cut{ui::Key{ui::Code::remove, ui::Mods{false, false, true}}};

    ought(sheet.dispatch(cut));

    same(one->text(), std::string{"ma kotaala "});
}

// Sam Delete bez shiftu ma dalej kasowac znak, a nie wycinac.
TRIAL("Editor: sam delete kasuje znak, a nie wycina") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    one->focus();
    one->cursor(0);

    ui::KeyEvent remove{ui::Code::remove};

    ought(sheet.dispatch(remove));
    same(one->text(), std::string{"bcdef"});
}

// --- Mysz ----------------------------------------------------------------------------------------

TRIAL("Editor: dwuklik bierze wyraz") {
    Sheet sheet;

    const auto one = shown(sheet, "ala ma kota\ndrugi");

    (void)sheet.render({{0, 0}, {20, 3}});

    ui::DoubleClickEvent what{{5, 0}, ui::Mouse::left};

    ought(sheet.dispatch(what));
    same(one->selection().text(), std::string{"ma"});
    same(one->selection().anchor(), std::size_t{4});
    same(one->cursor(), std::size_t{6});
}

// Ogonek jest literą tą samą regułą, po której grzebień szuka całych wyrazów — inaczej dwuklik
// i szukanie zaczęłyby się różnić.
TRIAL("Editor: dwuklik nie rozcina wyrazu z ogonkiem") {
    Sheet sheet;

    const auto one = shown(sheet, "ma k\xc4\x85t tam");

    (void)sheet.render({{0, 0}, {20, 2}});

    ui::DoubleClickEvent what{{3, 0}, ui::Mouse::left};

    ought(sheet.dispatch(what));
    same(one->selection().text(), std::string{"k\xc4\x85t"});
}

// Klik w miejsce, ktore wyrazem nie jest, bierze ciag takich samych — dwuklik nigdy nie wyglada
// na zignorowany.
TRIAL("Editor: dwuklik poza wyrazem bierze ciąg znaków tego samego rodzaju") {
    Sheet sheet;

    const auto one = shown(sheet, "ala   ma");

    (void)sheet.render({{0, 0}, {20, 2}});

    ui::DoubleClickEvent what{{4, 0}, ui::Mouse::left};

    ought(sheet.dispatch(what));
    same(one->selection().text(), std::string{"   "});
}

TRIAL("Editor: dwuklik za tabulatorem bierze wyraz po nim") {
    Sheet sheet;

    const auto one = shown(sheet, "\tala ma");

    (void)sheet.render({{0, 0}, {20, 2}});

    ui::DoubleClickEvent what{{9, 0}, ui::Mouse::left};

    ought(sheet.dispatch(what));
    same(one->selection().text(), std::string{"ala"});
}

// Wcisniecie stawia kursor razem z kotwica, ruch z wcisnietym przyciskiem rozciaga zaznaczenie.
TRIAL("Editor: ciągnięcie myszą rozciąga zaznaczenie") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef\nghijkl");

    (void)sheet.render({{0, 0}, {20, 3}});

    ui::MouseEvent down{{2, 0}, ui::Press::down, ui::Mouse::left};

    ought(sheet.dispatch(down));
    same(one->cursor(), std::size_t{2});
    ought(one->selection().empty());

    ui::MouseEvent move{{5, 0}, ui::Press::move, ui::Mouse::left};

    ought(sheet.dispatch(move));
    same(one->selection().anchor(), std::size_t{2});
    same(one->selection().text(), std::string{"cde"});

    ui::MouseEvent far{{3, 1}, ui::Press::move, ui::Mouse::left};

    ought(sheet.dispatch(far));
    same(one->selection().text(), std::string{"cdef\nghi"});

    ui::MouseEvent up{{3, 1}, ui::Press::up, ui::Mouse::left};

    ought(sheet.dispatch(up));

    // Po puszczeniu ruch nad edytorem nie ma juz prawa niczego rozciagac.
    ui::MouseEvent after{{5, 1}, ui::Press::move, ui::Mouse::none};

    ought(sheet.dispatch(after));
    same(one->selection().text(), std::string{"cdef\nghi"});
}

// Skrajny rzad przewija o wiersz, a szybkosc bierze sie z tego, ile uzytkownik naprawde rusza
// mysza — kiedy stanie, przewijanie staje razem z nim. Zegara do tego nie trzeba.
TRIAL("Editor: ciągnięcie w skrajnym rzędzie przewija") {
    Sheet sheet;

    const auto one = shown(sheet, many(20));

    (void)sheet.render({{0, 0}, {20, 3}});

    same(*one->first(), std::size_t{0});

    ui::MouseEvent down{{1, 0}, ui::Press::down, ui::Mouse::left};

    ought(sheet.dispatch(down));

    ui::MouseEvent low{{1, 2}, ui::Press::move, ui::Mouse::left};

    ought(sheet.dispatch(low));
    (void)sheet.render({{0, 0}, {20, 3}});
    same(*one->first(), std::size_t{1});

    ought(sheet.dispatch(low));
    (void)sheet.render({{0, 0}, {20, 3}});
    same(*one->first(), std::size_t{2});

    ought(!one->selection().empty());

    ui::MouseEvent high{{1, 0}, ui::Press::move, ui::Mouse::left};

    ought(sheet.dispatch(high));
    (void)sheet.render({{0, 0}, {20, 3}});
    same(*one->first(), std::size_t{1});
}

// Ciagniecie wychodzi poza edytor: wcisniecie prosi o przytrzymanie, wiec ruch pod edytorem
// i nad nim dochodzi dalej, przewija o wiersz i rozciaga zaznaczenie do skrajnego widocznego
// wiersza — poza obszarem nie ma kratki, w ktora mozna by trafic.
TRIAL("Editor: ciągnięcie poza edytor przewija i zaznacza do skrajnego wiersza") {
    Sheet sheet;

    const auto one = shown(sheet, many(20));

    (void)sheet.render({{0, 0}, {20, 3}});

    ui::MouseEvent down{{1, 0}, ui::Press::down, ui::Mouse::left};

    ought(sheet.dispatch(down));

    ui::MouseEvent below{{1, 10}, ui::Press::move, ui::Mouse::left};

    ought(sheet.dispatch(below));
    same(one->selection().text(), std::string{"0\nl1\nl"});

    (void)sheet.render({{0, 0}, {20, 3}});
    same(*one->first(), std::size_t{1});

    ought(sheet.dispatch(below));
    same(one->selection().text(), std::string{"0\nl1\nl2\nl"});

    (void)sheet.render({{0, 0}, {20, 3}});
    same(*one->first(), std::size_t{2});

    ui::MouseEvent above{{1, -5}, ui::Press::move, ui::Mouse::left};

    ought(sheet.dispatch(above));
    same(one->selection().text(), std::string{"0\nl1\nl"});

    (void)sheet.render({{0, 0}, {20, 3}});
    same(*one->first(), std::size_t{1});
}

// Puszczenie poza edytorem tez do niego wraca i konczy ciagniecie — dawniej szlo do tego, co
// lezalo pod kursorem, a zaznaczanie zostawalo zapalone do nastepnego wcisniecia.
TRIAL("Editor: puszczenie poza edytorem kończy ciągnięcie") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef\nghijkl");

    (void)sheet.render({{0, 0}, {20, 3}});

    ui::MouseEvent down{{1, 0}, ui::Press::down, ui::Mouse::left};
    ui::MouseEvent move{{4, 0}, ui::Press::move, ui::Mouse::left};
    ui::MouseEvent up{{4, 10}, ui::Press::up, ui::Mouse::left};

    ought(sheet.dispatch(down));
    ought(sheet.dispatch(move));
    (void)sheet.dispatch(up);

    same(one->selection().text(), std::string{"bcd"});

    ui::MouseEvent later{{3, 1}, ui::Press::move, ui::Mouse::left};

    (void)sheet.dispatch(later);

    same(one->selection().text(), std::string{"bcd"});
}

// `at()` znaczy w edytorze to samo, co w podglądzie: bajt, od którego zaczyna się pierwszy widoczny
// wiersz. Dawniej był to bajt wiersza **kursora** — tej wersji nikt nie wołał.
TRIAL("Editor: at to bajt pierwszego widocznego wiersza, nie wiersza kursora") {
    Sheet sheet;

    const auto one = shown(sheet, many(20));

    (void)sheet.render({{0, 0}, {20, 3}});

    one->cursor(std::size_t{0});
    one->first(std::size_t{4});

    same(one->cursor(), std::size_t{0});
    same(*one->at(), std::size_t{12});
    same(*one->whole(), many(20).size());
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
    same(one->selection().text(), std::string{"drugi"});
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

// Zamiana bierze zaznaczenie tylko wtedy, gdy **całe** jest trafieniem. Zaznaczenie, które trafienie
// tylko zawiera, zostaje nietknięte, a zamiana idzie szukać następnego.
TRIAL("Editor: zamiana nie bierze zaznaczenia, które trafienie tylko zawiera") {
    Sheet sheet;

    const auto one = shown(sheet, "xala ala");

    const Comb how{"ala", {}};

    one->selection().set(0, 4);

    ought(!one->replace(how, "ola"));
    same(one->text(), std::string{"xala ala"});
    same(one->selection().from(), std::size_t{5});
    same(one->selection().upto(), std::size_t{8});
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

    same(one->selection().text(), std::string{});

    ought(one->find(how, 0, false).has_value());
    same(one->selection().text(), std::string{"2026"});

    ought(one->find(how, 8, false).has_value());
    same(one->selection().text(), std::string{"12"});
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

    ought(!one->replace(how, "ola"));
    same(one->selection().text(), std::string{"ala"});
    same(one->text(), std::string{"ala ma ala"});
}

TRIAL("Editor: zamiana bierze zaznaczone dopasowanie i pokazuje następne") {
    Sheet sheet;

    const auto one = shown(sheet, "ala ma ala");

    const Comb how{"ala", {}};

    (void)one->find(how, 0, false);

    // Zamiana zostawia zaznaczone **nastepne** dopasowanie, wiec drugie wolanie zamienia je bez
    // szukania. Pomocnicze `whole` po drodze by to zepsulo — ono zaznacza caly dokument.
    ought(one->replace(how, "ola"));
    same(one->selection().text(), std::string{"ala"});

    ought(one->replace(how, "ola"));
    same(one->selection().text(), std::string{});

    same(one->text(), std::string{"ola ma ola"});
}

TRIAL("Editor: zamiana nie rusza zaznaczenia, które nie jest dopasowaniem") {
    Sheet sheet;

    const auto one = shown(sheet, "ala ma kota");

    const Comb how{"ala", {}};

    one->selection().set(4, 6);

    ought(!one->replace(how, "ola"));
    same(one->text(), std::string{"ala ma kota"});
}

TRIAL("Editor: zamiana wszystkich bierze każde dopasowanie") {
    Sheet sheet;

    const auto one = shown(sheet, "ala ma ala\nala kot");

    const Comb how{"ala", {}};

    same(one->replace_all(how, "ola"), std::size_t{3});
    same(one->text(), std::string{"ola ma ola\nola kot"});
}

// Wszystkie zamiany sa przywiazane do pierwszej, wiec cofaja sie jednym ruchem.
TRIAL("Editor: zamiana wszystkich cofa się jednym ruchem") {
    Sheet sheet;

    const auto one = shown(sheet, "ala ma ala\nala kot");

    const Comb how{"ala", {}};

    same(one->replace_all(how, "ola"), std::size_t{3});

    one->undo();

    same(one->text(), std::string{"ala ma ala\nala kot"});
    ought(!one->undoable());

    one->redo();

    same(one->text(), std::string{"ola ma ola\nola kot"});
}

// Wstawka niosaca w sobie wzorzec nie ma prawa zapetlic zamiany.
TRIAL("Editor: zamiana na tekst ze wzorcem w środku się nie zapętla") {
    Sheet sheet;

    const auto one = shown(sheet, "ala ala");

    const Comb how{"ala", {}};

    same(one->replace_all(how, "alala"), std::size_t{2});
    same(one->text(), std::string{"alala alala"});
}

TRIAL("Editor: zamiana na nic kasuje dopasowania") {
    Sheet sheet;

    const auto one = shown(sheet, "a1b22c333d");

    const Comb how{"[0-9]+", Sift{true, false, false}};

    same(one->replace_all(how, ""), std::size_t{3});
    same(one->text(), std::string{"abcd"});
}

TRIAL("Editor: zamiana po całych wyrazach") {
    Sheet sheet;

    const auto one = shown(sheet, "ala alama ala");

    const Comb how{"ala", Sift{false, false, true}};

    same(one->replace_all(how, "ola"), std::size_t{2});
    same(one->text(), std::string{"ola alama ola"});
}

// Zamiana wszystkich przesuwa blok razem z tekstem, stawia kursor za ostatnią wstawką, a cofnięcie
// oddaje i tekst, i blok, i kursor sprzed zamiany.
TRIAL("Editor: zamiana wszystkich przesuwa blok, stawia kursor i cofa się w całości") {
    Sheet sheet;

    const auto one = shown(sheet, "ala ma ala i ala");
    ui::Editor::Block& block = one->block();

    one->cursor(4);
    block.mark();
    one->cursor(6);
    block.mark();

    same(block.text(), std::string{"ma"});

    one->cursor(3);

    const Comb how{"ala", {}};

    same(one->replace_all(how, "kasia"), std::size_t{3});
    same(one->text(), std::string{"kasia ma kasia i kasia"});
    same(block.from(), std::size_t{6});
    same(block.upto(), std::size_t{8});
    same(one->cursor(), std::size_t{22});

    one->undo();

    same(one->text(), std::string{"ala ma ala i ala"});
    same(block.from(), std::size_t{4});
    same(block.upto(), std::size_t{6});
    same(one->cursor(), std::size_t{3});

    one->redo();

    same(one->text(), std::string{"kasia ma kasia i kasia"});
    same(one->cursor(), std::size_t{22});
}

// Wzorzec, który pasuje także do niczego, nie zapętla zamiany: puste trafienie nie zamienia nic
// i przesuwa szukanie o znak.
TRIAL("Editor: puste trafienie nie zapętla zamiany wszystkich") {
    Sheet sheet;

    const auto one = shown(sheet, "axxbx");

    const Comb how{"x*", Sift{true, false, false}};

    same(one->replace_all(how, "y"), std::size_t{2});
    same(one->text(), std::string{"ayby"});
}

// **Dwadzieścia tysięcy trafień zamienia się i cofa od razu.** Dawniej każde szło osobno przez
// szukanie, zaznaczenie i wstawienie, a kołdra przebudowywała się przy każdym — kwadrat.
TRIAL("Editor: dwadzieścia tysięcy trafień zamienia się i cofa jednym ruchem") {
    Sheet sheet;

    const std::string was = many(20000);
    const auto one = shown(sheet, was);

    const Comb how{"l", {}};

    same(one->replace_all(how, "wiersz "), std::size_t{20000});
    same(one->text().substr(0, 17), std::string{"wiersz 0\nwiersz 1"});
    same(one->text().size(), was.size() + 20000 * 6);

    one->undo();

    same(one->text(), was);
    ought(!one->undoable());

    one->redo();

    same(one->text().size(), was.size() + 20000 * 6);
}

// Wstawki z końcami wierszy zmieniają liczbę wierszy — skok do wiersza za dawnym końcem trafia.
TRIAL("Editor: zamiana wszystkich na wiersze poprawia liczbę wierszy") {
    Sheet sheet;

    const auto one = shown(sheet, "a\na\na");

    one->line(2);

    same(one->line(), std::size_t{2});

    const Comb how{"a", {}};

    same(one->replace_all(how, "x\ny"), std::size_t{3});

    one->line(5);

    same(one->line(), std::size_t{5});
    same(one->cursor(), std::size_t{10});
}

// Zamiana po cofnięciu poniżej zapisanego miejsca ucina gałąź, na której ono leżało — dokument jest
// brudny, choćby liczba wpisów wróciła do tej przy zapisie.
TRIAL("Editor: zamiana wszystkich po cofnięciu za zapis zostawia dokument brudny") {
    Sheet sheet;

    const auto one = shown(sheet, "ala");

    one->cursor(3);
    one->insert("!");
    one->clean();
    one->undo();

    const Comb how{"ala", {}};

    same(one->replace_all(how, "ola"), std::size_t{1});
    ought(one->dirty());
}

TRIAL("Editor: tylko do odczytu nie zamienia") {
    Sheet sheet;

    const auto one = shown(sheet, "ala ma ala");

    one->readonly(true);

    const Comb how{"ala", {}};

    same(one->replace_all(how, "ola"), std::size_t{0});
    same(one->text(), std::string{"ala ma ala"});
}

// --- Zapis ---------------------------------------------------------------------------------------

namespace {

// Ujście zbierające wszystko do napisu, z licznikiem kawałków — bo dokument większy od pamięci ma
// przepłynąć kawałkami, a nie jednym kęsem. Oddaje obietnicę spełnianą na puli, jak `File::write`.
// Pusty kawałek to koniec: liczy się osobno, bo to on domyka zapis.
Drain into(std::string& what, int& much, int* ended = nullptr) {
    return [&what, &much, ended](Bytes got) {
        if (got.empty() && ended != nullptr) ++*ended;

        for (const std::byte one : got) what.push_back(static_cast<char>(one));

        if (!got.empty()) ++much;

        return Promise<void>{[](auto resolve, auto) { resolve(); }};
    };
}

// Obraca pętlę, aż obietnica zapisu się rozstrzygnie; oddaje, czy się spełniła. Ciąg dalszy
// idzie **na pętlę** (`on`): zapis kończy się na wątku puli, a zmienną czyta ten wątek. Bez tego
// odrzucony zapis wpisywał wynik z puli i sanitizer wątków łapał wyścig.
bool saved(Loop& loop, Promise<void> made) {
    std::optional<bool> went;

    Promise<void> watched =
        made.on(loop).then([&went] { went = true; }).fail([&went](const std::exception&) { went = false; });

    watched.start();

    for (int tries = 0; tries < 4000 && !went.has_value(); ++tries) {
        (void)loop.bell().wait(std::chrono::milliseconds{5});
        (void)loop.turn();
    }

    return went.value_or(false);
}

}

TRIAL("Editor: zapis oddaje całą treść, kończy pustym kawałkiem i czyści dokument") {
    Loop loop;
    Sheet sheet;

    const auto one = shown(sheet, "abc\ndef");

    one->cursor(0);
    one->insert("X");

    ought(one->dirty());

    std::string got;
    int much = 0;
    int ended = 0;

    Promise<void> made = one->save(into(got, much, &ended));

    // Do spełnienia edytor jest zajęty i brudny — pierze się dopiero po całości.
    ought(one->busy());
    ought(one->dirty());

    ought(saved(loop, std::move(made)));

    same(got, std::string{"Xabc\ndef"});
    same(much, 1);
    same(ended, 1);
    ought(!one->dirty());
    ought(!one->busy());
}

// Pusty dokument to samo jedno wołanie końca: ujście ma co domknąć, choć nie ma czego pisać.
TRIAL("Editor: zapis pustego dokumentu oddaje tylko koniec") {
    Loop loop;
    Sheet sheet;

    const auto one = shown(sheet, "");

    std::string got;
    int much = 0;
    int ended = 0;

    ought(saved(loop, one->save(into(got, much, &ended))));

    same(got, std::string{});
    same(much, 0);
    same(ended, 1);
}

// „Czysty" znaczy „na dysku": ujście, które odmawia dopiero przy końcu — bo nie dało się domknąć
// pliku — zostawia dokument brudny, choć wszystkie kawałki przyjęło.
TRIAL("Editor: odmowa przy końcu zostawia dokument brudny") {
    Loop loop;
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    one->cursor(0);
    one->insert("X");

    std::string got;

    ought(!saved(loop, one->save([&got](Bytes given) {
        if (given.empty())
            return Promise<void>{[](auto, auto reject) { reject(std::make_exception_ptr(std::runtime_error{"rename failed"})); }};

        for (const std::byte one : given) got.push_back(static_cast<char>(one));

        return Promise<void>{[](auto resolve, auto) { resolve(); }};
    })));

    same(got, std::string{"Xabcdef"});
    ought(one->dirty());
    ought(!one->busy());
}

// Niedokończony zapis nie jest zapisem: dokument zostaje brudny, żeby aplikacja nie uznała go
// za odłożony na dysk. Ujście bez obietnicy i drugi zapis w trakcie pierwszego są odrzucane od
// razu.
TRIAL("Editor: ujście, które odmówi, przerywa zapis") {
    Loop loop;
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    one->cursor(0);
    one->insert("X");

    ought(!saved(loop, one->save([](Bytes) {
        return Promise<void>{[](auto, auto reject) { reject(std::make_exception_ptr(std::runtime_error{"disk full"})); }};
    })));
    ought(one->dirty());
    ought(!one->busy());

    ought(!saved(loop, one->save({})));

    std::string got;
    int much = 0;

    Promise<void> first = one->save(into(got, much));

    ought(!saved(loop, one->save(into(got, much))));
    ought(saved(loop, std::move(first)));
    ought(!one->dirty());
}

// Ujście, które rzuca, zanim odda obietnicę: zapis jest odrzucony, a edytor nie zostaje zajęty — inaczej
// każdy następny zapis byłby odrzucany jako „drugi w trakcie", a wyjście z edytora czekało bez końca.
TRIAL("Editor: ujście rzucające przed obietnicą nie zostawia edytora zajętym") {
    Loop loop;
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    one->cursor(0);
    one->insert("X");

    bool sync = false;
    std::optional<Promise<void>> made;

    try {
        made.emplace(one->save([](Bytes) -> Promise<void> { throw std::runtime_error{"no handle"}; }));
    } catch (const std::exception&) {
        sync = true;
    }

    ought(!sync);
    ought(made.has_value() && !saved(loop, std::move(*made)));
    ought(!one->busy());
    ought(one->dirty());

    std::string got;
    int much = 0;

    ought(saved(loop, one->save(into(got, much))));
    same(got, std::string{"Xabcdef"});
}

// W trakcie zapisu treść stoi: klawisze zmieniające ją odbijają się, cofanie też, a po
// spełnieniu wszystko wraca.
TRIAL("Editor: w trakcie zapisu treść nie zmienia się") {
    Loop loop;
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    one->cursor(0);
    one->insert("X");

    std::string got;
    int much = 0;

    Promise<void> made = one->save(into(got, much));

    one->cursor(0);
    one->insert("Y");

    same(one->text(), std::string{"Xabcdef"});
    ought(!one->undoable());

    ought(saved(loop, std::move(made)));

    one->cursor(0);
    one->insert("Y");

    same(one->text(), std::string{"YXabcdef"});
    ought(one->undoable());
}

TRIAL("Editor: duży dokument płynie kawałkami") {
    Loop loop;
    Sheet sheet;

    std::string big;

    for (int which = 0; which < 20000; ++which) big += "wiersz numer " + std::to_string(which) + "\n";

    const auto one = shown(sheet, big);

    std::string got;
    int much = 0;

    ought(saved(loop, one->save(into(got, much))));

    same(got, big);
    ought(much > 1);
}

// --- Czysty czy brudny --------------------------------------------------------------------------

// Dwa kłamstwa `dirty()`, które liczyło same numery wpisów: wyraz pisany dalej po zapisie
// przedłużał ten sam wpis, a nowa zmiana po cofnięciu poniżej zapisanego miejsca dostawała ten sam
// numer, co zapisane. Zapis pieczętuje wpis, ucięcie gałęzi unieważnia zapamiętany numer.
TRIAL("Editor: pisanie dalej po zapisie brudzi dokument") {
    Loop loop;
    Sheet sheet;

    const auto one = shown(sheet, "abc");

    one->cursor(3);
    one->insert("x");

    std::string got;
    int much = 0;

    ought(saved(loop, one->save(into(got, much))));
    ought(!one->dirty());

    one->insert("y");

    ought(one->dirty());
    same(one->text(), std::string{"abcxy"});

    // Cofnięcie samego `y` wraca do zapisanego — bo `y` zaczęło własny wpis.
    one->undo();

    ought(!one->dirty());
    same(one->text(), std::string{"abcx"});
}

TRIAL("Editor: nowa zmiana po cofnięciu poniżej zapisu brudzi dokument na zawsze") {
    Loop loop;
    Sheet sheet;

    const auto one = shown(sheet, "abc");

    one->cursor(3);
    one->insert("x");
    one->cursor(0);
    one->insert("w");

    std::string got;
    int much = 0;

    ought(saved(loop, one->save(into(got, much))));
    ought(!one->dirty());

    one->undo();

    ought(one->dirty());

    one->cursor(0);
    one->insert("q");

    ought(one->dirty());
    same(one->text(), std::string{"qabcx"});

    // Żadne cofanie nie wraca już do zapisanego stanu.
    one->undo();
    one->undo();

    ought(one->dirty());
}

// --- Kliknięcie ----------------------------------------------------------------------------------

TRIAL("Editor: kliknięcie stawia kursor w klikniętej kratce") {
    Sheet sheet;

    const auto one = shown(sheet, "abc\ndef");

    (void)sheet.render({{0, 0}, {20, 3}});

    ui::ClickEvent what{{2, 1}, ui::Mouse::left};

    ought(sheet.dispatch(what));
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

    ought(sheet.dispatch(what));
    same(one->cursor(), std::size_t{5});
    same(one->line(), std::size_t{0});
}

// Kolumna nie jest bajtem: tabulator zajmuje osiem kratek, a wszystkie wskazują ten sam bajt.
TRIAL("Editor: kliknięcie za tabulatorem trafia we właściwy bajt") {
    Sheet sheet;

    const auto one = shown(sheet, "\tab");

    (void)sheet.render({{0, 0}, {20, 2}});

    ui::ClickEvent what{{8, 0}, ui::Mouse::left};

    ought(sheet.dispatch(what));
    same(one->cursor(), std::size_t{1});

    ui::ClickEvent far{{3, 0}, ui::Mouse::left};

    ought(sheet.dispatch(far));
    same(one->cursor(), std::size_t{0});
}

TRIAL("Editor: kliknięcie za końcem wiersza staje na jego końcu") {
    Sheet sheet;

    const auto one = shown(sheet, "abc\ndef");

    (void)sheet.render({{0, 0}, {20, 3}});

    ui::ClickEvent what{{15, 0}, ui::Mouse::left};

    ought(sheet.dispatch(what));
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

    ought(one->selection().empty());

    one->selection().extend(5);

    ought(!one->selection().empty());
    same(one->selection().anchor(), std::size_t{2});
    same(one->cursor(), std::size_t{5});
    same(one->selection().text(), std::string{"cde"});

    one->cursor(4);

    ought(one->selection().empty());
}

TRIAL("Editor: shift ze strzałką rozszerza zaznaczenie") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    one->focus();
    one->cursor(2);

    ui::KeyEvent right{ui::Key{ui::Code::right, ui::Mods{false, false, true}}};

    ought(sheet.dispatch(right));
    ought(sheet.dispatch(right));

    same(one->selection().anchor(), std::size_t{2});
    same(one->cursor(), std::size_t{4});
    same(one->selection().text(), std::string{"cd"});
}

TRIAL("Editor: shift z ruchem w dół bierze zaznaczenie przez wiersze") {
    Sheet sheet;

    const auto one = shown(sheet, "abc\ndef");

    one->focus();

    (void)sheet.render({{0, 0}, {20, 3}});

    one->cursor(1);

    ui::KeyEvent down{ui::Key{ui::Code::down, ui::Mods{false, false, true}}};

    ought(sheet.dispatch(down));
    same(one->selection().text(), std::string{"bc\nd"});
}

TRIAL("Editor: shift z Home bierze początek wiersza") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    one->focus();
    one->cursor(4);

    ui::KeyEvent home{ui::Key{ui::Code::home, ui::Mods{false, false, true}}};

    ought(sheet.dispatch(home));
    same(one->selection().text(), std::string{"abcd"});
    same(one->selection().anchor(), std::size_t{4});
}

// --- Stronicowanie -------------------------------------------------------------------------------

TRIAL("Editor: klawisz strony przenosi kursor o ekran") {
    Sheet sheet;

    const auto one = shown(sheet, many(10));

    one->focus();

    (void)sheet.render({{0, 0}, {20, 3}});

    ui::KeyEvent down{ui::Code::pagedown};

    ought(sheet.dispatch(down));
    same(one->line(), std::size_t{3});

    ought(sheet.dispatch(down));
    same(one->line(), std::size_t{6});

    ui::KeyEvent up{ui::Code::pageup};

    ought(sheet.dispatch(up));
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

    ought(sheet.dispatch(down));
    same(one->line(), std::size_t{9});

    ought(!sheet.dispatch(down));
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

    ought(!sheet.dispatch(up));

    one->cursor(5);

    ui::KeyEvent down{ui::Code::down};

    ought(!sheet.dispatch(down));
}

TRIAL("Editor: F-klawisze przechodzą wyżej") {
    Sheet sheet;

    const auto one = shown(sheet, "abc");

    one->focus();

    ui::KeyEvent f2{ui::Code::f2};

    ought(!sheet.dispatch(f2));
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

    same(one->text(), std::string{"Yaaa\nXbbb\nccc\ndddZ"});
    same(*one->lines(), std::size_t{4});
}

// **Trzymany Backspace i Delete przez granicę wpisanego tekstu.** Kasowane znaki leżą raz
// w dopiskach, raz w oryginale, więc wpis zbiera odcinki z dwóch źródeł — i dalej jest jednym
// wpisem, który wraca w dobrej kolejności.
TRIAL("Editor: kasowanie przez granicę wpisanego tekstu cofa się jednym ruchem") {
    Sheet sheet;

    const auto one = shown(sheet, "abcdef");

    one->cursor(3);
    one->insert("XY");
    one->cursor(5);

    for (int step = 0; step < 4; ++step) one->back_erase(1);

    same(one->text(), std::string{"adef"});

    one->undo();

    same(one->text(), std::string{"abcXYdef"});

    one->redo();

    same(one->text(), std::string{"adef"});

    one->undo();
    one->cursor(2);

    for (int step = 0; step < 4; ++step) one->front_erase(1);

    same(one->text(), std::string{"abef"});

    one->undo();

    same(one->text(), std::string{"abcXYdef"});

    one->undo();

    same(one->text(), std::string{"abcdef"});
    ought(!one->undoable());
}

// **Wpisy dziennika to skrawki kołdry.** Losowe wpisywanie, kasowanie w obie strony i bloki (F5,
// F6, F8), a potem cofnięcie wszystkiego krok po kroku i ponowienie wszystkiego — po każdym kroku
// tekst taki, jaki był. Skrawki kopiowanego i przenoszonego bloku wskazują te same odcinki co
// dokument, więc gdyby któraś zmiana je psuła, wyszłoby to przy cofaniu.
TRIAL("Editor: losowe bloki i kasowania cofają się i ponawiają") {
    std::mt19937 gen(5);

    const auto drawn = [&gen](std::size_t much) { return much == 0 ? 0 : gen() % (much + 1); };

    for (int round = 0; round < 60; ++round) {
        Sheet sheet;

        const std::string was = many(40);

        const auto one = shown(sheet, was);

        ui::Editor::Block& block = one->block();

        std::vector<std::string> states{was};

        for (int step = 0; step < 15; ++step) {
            const std::string before = one->text();
            const std::size_t much = before.size();
            const unsigned what = gen() % 6;

            if (what == 0 || much < 2) {
                std::string more;

                const int size = 1 + static_cast<int>(gen() % 6);

                for (int which = 0; which < size; ++which)
                    more.push_back(gen() % 5 == 0 ? '\n' : static_cast<char>('A' + gen() % 26));

                one->cursor(drawn(much));
                one->insert(more);
            } else if (what == 1) {
                one->cursor(drawn(much));
                one->back_erase(1 + gen() % 20);
            } else if (what == 2) {
                one->cursor(drawn(much));
                one->front_erase(1 + gen() % 20);
            } else {
                const std::size_t from = gen() % (much - 1);
                const std::size_t upto = from + 1 + gen() % (much - from - 1);

                block.clear();
                one->cursor(from);
                block.mark();
                one->cursor(upto);
                block.mark();
                one->cursor(drawn(much));

                if (what == 3) (void)block.copy();
                if (what == 4) (void)block.move();
                if (what == 5) (void)block.erase();
            }

            if (one->text() != before) states.push_back(one->text());
        }

        for (std::size_t at = states.size() - 1; at > 0; --at) {
            one->undo();

            same(one->text(), states[at - 1]);
        }

        ought(!one->undoable());

        for (std::size_t at = 1; at < states.size(); ++at) {
            one->redo();

            same(one->text(), states[at]);
        }

        ought(!one->redoable());
    }
}

// Duży blok skasowany, przeniesiony i skopiowany, a potem wiele razy cofnięty i ponowiony: tekst
// wraca co do bajtu. Dawniej każde cofnięcie wstawiało kopię bloku do dopisków kołdry, które nigdy
// się nie kurczą.
TRIAL("Editor: duży blok cofa się i ponawia wiele razy") {
    Sheet sheet;

    const std::string was = many(100000);

    const auto one = shown(sheet, was);

    ui::Editor::Block& block = one->block();

    one->cursor(10);
    block.mark();
    one->cursor(was.size() - 10);
    block.mark();

    one->cursor(was.size());

    ought(block.copy());

    const std::string copied = one->text();

    ought(block.erase());

    const std::string erased = one->text();

    same(erased, was.substr(0, 10) + was.substr(was.size() - 10) + was.substr(10, was.size() - 20));

    for (int round = 0; round < 20; ++round) {
        one->undo();

        same(one->text().size(), copied.size());

        one->undo();

        same(one->text(), was);

        one->redo();
        one->redo();

        same(one->text(), erased);
    }
}

// Enter skasowany Backspace'em albo Delete'em zaczyna nowy krok cofania — tak samo jak wpisany.
// Cofnięcie zabiera wtedy najpierw połączenie wierszy, a dopiero potem znaki przed nim.
TRIAL("Editor: skasowany koniec wiersza jest osobnym krokiem") {
    Sheet sheet;

    const auto one = shown(sheet, "ab\ncd");

    one->cursor(5);
    one->back_erase(1);
    one->back_erase(1);
    one->back_erase(1);

    same(one->text(), std::string{"ab"});

    one->undo();

    same(one->text(), std::string{"ab\n"});

    one->undo();

    same(one->text(), std::string{"ab\ncd"});

    one->cursor(0);
    one->front_erase(1);
    one->front_erase(1);
    one->front_erase(1);

    same(one->text(), std::string{"cd"});

    one->undo();

    same(one->text(), std::string{"\ncd"});
}
