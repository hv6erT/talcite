#include "proving.hpp"

#include <memory>
#include <string>

#include "talcite/button.hpp"
#include "talcite/column.hpp"
#include "talcite/dialog.hpp"
#include "talcite/veilwindow.hpp"
#include "talcite/encoding.hpp"
#include "talcite/field.hpp"
#include "talcite/frame.hpp"
#include "talcite/root.hpp"
#include "talcite/scroll.hpp"
#include "talcite/skin.hpp"

using namespace talcite;

#include "doing.hpp"

namespace {

std::string read(const View& one, int y) {
    std::string made;

    for (int x = 0; x < one.size().wide; ++x) {
        const char32_t letter = one.at({x, y}).letter;

        if (letter != 0) talcite::spelled(letter, made);
    }

    while (!made.empty() && made.back() == ' ') made.pop_back();

    return made;
}

// Wpisywanie idzie **klawiszami**, bo tak wpisuje użytkownik: próba sprawdza wtedy tę samą drogę,
// która naprawdę biegnie, a nie skrót obok niej.
void typed(ui::Root& top, const std::string& what) {
    std::size_t at = 0;

    while (at < what.size()) {
        ui::KeyEvent one{letter(what, at)};

        (void)top.deal(one);
    }
}

void pressed(ui::Root& top, ui::Code code, ui::Mods mods = {}) {
    ui::KeyEvent one{ui::Key{code, mods}};

    (void)top.deal(one);
}

}

TRIAL("Pole: wpisywanie stawia znaki przy kursorze") {
    ui::Root top;

    const auto one = std::make_shared<ui::Field>(10);

    top.children().add(one);

    one->focus();

    typed(top, "abc");

    same(one->text(), std::string{"abc"});
    same(one->cursor(), static_cast<std::size_t>(3));

    pressed(top, ui::Code::left);
    pressed(top, ui::Code::left);

    typed(top, "X");

    same(one->text(), std::string{"aXbc"});
    same(one->cursor(), static_cast<std::size_t>(2));

    same(read(top.render({{0, 0}, {10, 1}}), 0), std::string{"aXbc"});
}

TRIAL("Pole: Backspace kasuje przed kursorem, Delete pod nim") {
    ui::Root top;

    const auto one = std::make_shared<ui::Field>(10, "abcd");

    top.children().add(one);

    one->focus();

    pressed(top, ui::Code::backspace);

    same(one->text(), std::string{"abc"});

    pressed(top, ui::Code::home);
    pressed(top, ui::Code::remove);

    same(one->text(), std::string{"bc"});
    same(one->cursor(), static_cast<std::size_t>(0));

    // Na krawędziach nie ma czego kasować i nic się nie dzieje.
    pressed(top, ui::Code::backspace);

    same(one->text(), std::string{"bc"});

    pressed(top, ui::Code::end);
    pressed(top, ui::Code::remove);

    same(one->text(), std::string{"bc"});
}

TRIAL("Pole: kursor liczy się w znakach, nie w bajtach") {
    ui::Root top;

    const auto one = std::make_shared<ui::Field>(10, "zażółć");

    top.children().add(one);

    one->focus();

    // Sześć znaków, choć bajtów więcej.
    same(one->cursor(), static_cast<std::size_t>(6));

    pressed(top, ui::Code::left);

    same(one->cursor(), static_cast<std::size_t>(5));

    pressed(top, ui::Code::backspace);

    same(one->text(), std::string{"zażóć"});

    one->cursor(2);

    same(one->cursor(), static_cast<std::size_t>(2));

    typed(top, "X");

    same(one->text(), std::string{"zaXżóć"});
}

TRIAL("Pole: wklejenie wchodzi przy kursorze, końce wierszy jako odstępy") {
    ui::Root top;

    const auto one = std::make_shared<ui::Field>(20, "ab");

    top.children().add(one);

    one->focus();

    ui::PasteEvent what{"raz\ndwa"};

    ought(top.deal(what));

    same(one->text(), std::string{"abraz dwa"});
    same(one->cursor(), static_cast<std::size_t>(9));
}

// Pole **się przewija, nie ucina**: wielokropek w treści, którą się właśnie pisze, byłby kłamstwem
// o tym, co w niej stoi.
TRIAL("Pole: przewija się tak, żeby kursor był widoczny") {
    ui::Root top;

    const auto one = std::make_shared<ui::Field>(5, "abcdefgh");

    top.children().add(one);

    one->focus();

    const View made = top.render({{0, 0}, {5, 1}});

    same(read(made, 0), std::string{"efgh"});

    ought(top.caret().has_value());
    same(*top.caret(), (ui::Spot{4, 0}));

    // Powrót na początek przewija z powrotem.
    pressed(top, ui::Code::home);

    const View other = top.render({{0, 0}, {5, 1}});

    same(read(other, 0), std::string{"abcde"});
    same(*top.caret(), (ui::Spot{0, 0}));
}

TRIAL("Pole: znak dwukolumnowy zajmuje dwie kratki") {
    ui::Root top;

    const auto one = std::make_shared<ui::Field>(6, "日本ab");

    top.children().add(one);

    one->focus();

    const View made = top.render({{0, 0}, {6, 1}});

    // Sześć kratek treści nie mieści kursora za nią, więc pierwszy znak wychodzi z widoku.
    same(read(made, 0), std::string{"本ab"});
    same(*top.caret(), (ui::Spot{4, 0}));
}

TRIAL("Pole: kursor widać tylko pod ogniskiem") {
    ui::Root top;

    const auto one = std::make_shared<ui::Field>(6, "ab");

    top.children().add(one);

    (void)top.render({{0, 0}, {6, 1}});

    ought(!top.caret().has_value());

    one->focus();

    (void)top.render({{0, 0}, {6, 1}});

    ought(top.caret().has_value());
}

// **Zdarzenie myszy niesie kratkę ekranu, a nie własną.** Odjęcie samego `area().at` liczyło tylko
// jedno piętro, więc pole w ramce w ramce — czyli w każdym oknie dialogowym — stawiało kursor o tyle
// kratek obok, ile wynosiło przesunięcie przodków.
TRIAL("Pole: klik trafia we właściwy znak także w głębi drzewa") {
    ui::Root top;

    const auto outer = std::make_shared<ui::Frame>("A");
    const auto inner = std::make_shared<ui::Frame>("B");
    const auto one = std::make_shared<ui::Field>(20, "abcdefgh");

    inner->children().add(one);
    outer->children().add(inner);
    top.children().add(outer);

    (void)top.render({{0, 0}, {30, 8}});

    // Dwie ramki po jednej kratce z każdej strony, więc znak nr 3 stoi w ekranowej kolumnie 5.
    same(one->area().at, (ui::Spot{1, 1}));

    ui::ClickEvent what{{5, 2}, ui::Mouse::left};

    ought(top.deal(what));
    same(one->cursor(), static_cast<std::size_t>(3));
}

TRIAL("Pole: klik stawia kursor tam, gdzie kliknięto") {
    ui::Root top;

    const auto one = std::make_shared<ui::Field>(10, "abcdef");

    top.children().add(one);

    (void)top.render({{0, 0}, {10, 1}});

    ui::ClickEvent what{{3, 0}, ui::Mouse::left};

    ought(top.deal(what));

    same(one->cursor(), static_cast<std::size_t>(3));
    same(top.focus(), static_cast<ui::Node*>(one.get()));

    // Klik za treścią stawia kursor na jej końcu.
    ui::ClickEvent aside{{9, 0}, ui::Mouse::left};

    ought(top.deal(aside));

    same(one->cursor(), static_cast<std::size_t>(6));
}

TRIAL("Pole: wyłączone nie bierze ogniska i nie przyjmuje znaków") {
    ui::Root top;

    const auto one = std::make_shared<ui::Field>(10, "ab");

    top.children().add(one);

    one->able(false);

    ought(!one->focusable());

    one->focus();

    same(top.focus(), static_cast<ui::Node*>(nullptr));

    typed(top, "c");

    same(one->text(), std::string{"ab"});
}

// Podpowiedź jest **wyszarzonym dopiskiem za treścią** i nie należy do treści, dopóki jej ktoś
// nie przyjmie.
TRIAL("Podpowiedź: rysuje się wyszarzona i nie wchodzi do treści") {
    ui::Root top;

    const auto one = std::make_shared<ui::Field>(12);

    one->guess([](const std::string& what) {
        return what.empty() ? std::string{} : std::string{"katalog"};
    });

    top.children().add(one);

    one->focus();

    typed(top, "ka");

    same(one->text(), std::string{"ka"});
    same(one->guessed(), std::string{"talog"});

    const View made = top.render({{0, 0}, {12, 1}});

    same(read(made, 0), std::string{"katalog"});

    // Treść zwykła, dopisek przygaszony.
    ought(!made.at({1, 0}).style.faint);
    ought(made.at({2, 0}).style.faint);
    ought(made.at({6, 0}).style.faint);

    // Kursor stoi na końcu **treści**, a nie podpowiedzi.
    same(*top.caret(), (ui::Spot{2, 0}));
}

TRIAL("Podpowiedź: strzałka w prawo dobiera jedną literę") {
    ui::Root top;

    const auto one = std::make_shared<ui::Field>(12);

    one->guess([](const std::string&) { return std::string{"katalog"}; });

    top.children().add(one);

    one->focus();

    typed(top, "ka");

    pressed(top, ui::Code::right);

    same(one->text(), std::string{"kat"});
    same(one->guessed(), std::string{"alog"});

    pressed(top, ui::Code::right);

    same(one->text(), std::string{"kata"});
}

// **Taba pole nie bierze.** Dobierał całą podpowiedź, ale tylko wtedy, gdy podpowiedź była — więc
// ten sam klawisz raz przechodził do następnego widżetu, a raz nie, zależnie od treści pola.
// Klawisz wędrówki nie może znikać zależnie od zawartości.
TRIAL("Podpowiedź: Tab jej nie uzupełnia i idzie wyżej") {
    ui::Root top;

    const auto one = std::make_shared<ui::Field>(12);

    one->guess([](const std::string&) { return std::string{"katalog"}; });

    top.children().add(one);

    one->focus();

    typed(top, "ka");

    ui::KeyEvent tab{ui::Code::tab};

    ought(!top.deal(tab));

    same(one->text(), std::string{"ka"});
    same(one->guessed(), std::string{"talog"});
}

TRIAL("Podpowiedź: pojawia się tylko z kursorem na końcu") {
    ui::Root top;

    const auto one = std::make_shared<ui::Field>(12);

    one->guess([](const std::string&) { return std::string{"katalog"}; });

    top.children().add(one);

    one->focus();

    typed(top, "ka");

    ought(!one->guessed().empty());

    pressed(top, ui::Code::left);

    ought(one->guessed().empty());

    pressed(top, ui::Code::end);

    ought(!one->guessed().empty());
}

TRIAL("Podpowiedź: niepasująca do treści odpada") {
    ui::Root top;

    const auto one = std::make_shared<ui::Field>(12);

    // Podpowiedź, która nie zaczyna się od tego, co napisano, nie ma jak zostać dopisana.
    one->guess([](const std::string&) { return std::string{"inne"}; });

    top.children().add(one);

    one->focus();

    typed(top, "ka");

    ought(one->guessed().empty());

    ui::KeyEvent tab{ui::Code::tab};

    // Bez podpowiedzi Tab do pola nie należy.
    ought(!top.deal(tab));
}

namespace {

struct Asked {
    std::shared_ptr<ui::VeilWindow> window;
    std::shared_ptr<ui::Dialog> dialog;
    std::shared_ptr<ui::Field> field;
    std::shared_ptr<ui::Button> yes;
    std::shared_ptr<ui::Button> no;
};

Asked naming(ui::Root& top, int* done) {
    Asked made;

    made.window = std::make_shared<ui::VeilWindow>();
    made.dialog = std::make_shared<ui::Dialog>();
    made.field = std::make_shared<ui::Field>(12);
    made.yes = proving::doing(std::make_shared<ui::Button>("Tak"), [done] { ++*done; });
    made.no = std::make_shared<ui::Button>("Nie");

    made.yes->keys().add(U'T');
    made.no->keys().add(U'N');
    made.yes->primary(true);

    const auto column = std::make_shared<ui::Column>();

    column->children().add(made.field);
    column->children().add(made.yes);
    column->children().add(made.no);
    made.dialog->children().add(column);

    top.children().add(made.window);
    made.window->open(made.dialog);

    made.field->focus();

    (void)top.render({{0, 0}, {30, 10}});

    return made;
}

}

// **To jest zderzenie, dla którego pole powstało teraz, a nie później.** Litera w dialogu naciska
// przycisk, który ma ją w klawiszach aktywnych — ale pole z ogniskiem musi ją wpisać. Bąbelkowanie
// rozstrzyga to samo, bo pole leży bliżej ogniska, ale reguła musi być sprawdzona.
TRIAL("Dialog: litera w polu wpisuje się, a nie naciska przycisku") {
    ui::Root top;

    int done = 0;

    const Asked asked = naming(top, &done);

    typed(top, "t");

    same(asked.field->text(), std::string{"t"});
    same(done, 0);

    // Bez ogniska w polu ta sama litera naciska przycisk.
    asked.no->focus();

    typed(top, "t");

    same(asked.field->text(), std::string{"t"});
    same(done, 1);
}

// **Pole przepuszcza Enter, a okno go nie przechwytuje.** Nikt więc nie zatwierdza formularza za
// wołającego — kto tego chce, wiesza uchwyt na polu i sam decyduje, co znaczy „gotowe". Zasłona
// klawisz połyka, bo jest modalna, ale nic nim nie naciska.
TRIAL("Dialog: Enter w polu nie naciska żadnego przycisku") {
    ui::Root top;

    int done = 0;

    const Asked asked = naming(top, &done);

    typed(top, "nazwa");

    ui::KeyEvent enter{ui::Code::enter};

    ought(top.deal(enter));

    same(done, 0);
    same(asked.field->text(), std::string{"nazwa"});
}

// Tak wygląda to samo napisane po nowemu: czynność „gotowe" wisi tam, gdzie stoi ognisko.
TRIAL("Dialog: Enter w polu robi to, co dopiął wołający") {
    ui::Root top;

    int done = 0;

    const Asked asked = naming(top, &done);

    int ready = 0;

    (void)asked.field->on<ui::KeyEvent>([&ready](ui::KeyEvent& what) {
        if (what.key() != ui::Code::enter) return false;

        ++ready;

        return true;
    });

    typed(top, "nazwa");

    ui::KeyEvent enter{ui::Code::enter};

    ought(top.deal(enter));

    same(ready, 1);
    same(asked.field->text(), std::string{"nazwa"});
}

// Esc w polu **nie zamyka okna** — pole go nie bierze, a zasłona tylko połyka. Kto chce zamykanie,
// wiesza uchwyt na swoim oknie.
TRIAL("Dialog: Esc w polu okna nie zamyka") {
    ui::Root top;

    int done = 0;

    const Asked asked = naming(top, &done);

    ui::KeyEvent what{ui::Code::escape};

    ought(top.deal(what));

    ought(!asked.window->veils().empty());
}

// Tab należy do wędrówki ogniska i tylko do niej — pole przestało go brać, więc nie ma już
// miejsca, w którym te dwa znaczenia Taba się spotykają.
TRIAL("Dialog: Tab w polu bez podpowiedzi przechodzi do przycisku") {
    ui::Root top;

    int done = 0;

    const Asked asked = naming(top, &done);

    ui::KeyEvent tab{ui::Code::tab};

    ought(top.deal(tab));

    same(top.focus(), static_cast<ui::Node*>(asked.yes.get()));
}

// Tab **zawsze** przenosi ognisko, także wtedy, gdy pole ma co uzupełniać. Wcześniej pierwszy Tab
// uzupełniał, a dopiero drugi wędrował — czyli wędrówka po oknie zależała od treści pola.
TRIAL("Dialog: Tab w polu z podpowiedzią przenosi ognisko od razu") {
    ui::Root top;

    int done = 0;

    const Asked asked = naming(top, &done);

    asked.field->guess([](const std::string&) { return std::string{"nazwa"}; });

    typed(top, "na");

    ui::KeyEvent tab{ui::Code::tab};

    ought(top.deal(tab));

    same(asked.field->text(), std::string{"na"});
    same(top.focus(), static_cast<ui::Node*>(asked.yes.get()));
}

// **Pole liczbowe to jedna lambda, a nie osobny widżet.** Midnight Commander nie ma widżetu
// liczbowego — liczby wpisuje w zwykłe `input` — więc nie ma tu czego naśladować.
TRIAL("Pole: filtr odsiewa znaki przy pisaniu") {
    ui::Root top;

    const auto one = std::make_shared<ui::Field>(10);

    one->allows([](char32_t which) { return which >= U'0' && which <= U'9'; });

    top.children().add(one);

    one->focus();

    typed(top, "a1b2c3");

    same(one->text(), std::string{"123"});
    same(one->cursor(), static_cast<std::size_t>(3));
}

TRIAL("Pole: filtr odsiewa przy wklejaniu, resztę wpuszcza") {
    ui::Root top;

    const auto one = std::make_shared<ui::Field>(10);

    one->allows([](char32_t which) { return which >= U'0' && which <= U'9'; });

    top.children().add(one);

    one->focus();

    ui::PasteEvent what{"12ab34"};

    ought(top.deal(what));

    // Jeden zły znak w środku nie unieważnia całego wklejenia.
    same(one->text(), std::string{"1234"});
}

// Bez filtra pole przyjmuje wszystko — filtr jest dodatkiem, nie warunkiem działania.
TRIAL("Pole: bez filtra wchodzi wszystko") {
    ui::Root top;

    const auto one = std::make_shared<ui::Field>(10);

    top.children().add(one);

    one->focus();

    typed(top, "a1b2");

    same(one->text(), std::string{"a1b2"});
}

// Ustawienie treści programem **filtra nie pyta**: wołający wie, co robi, a odsiew jest po to,
// żeby pilnować tego, co wpisuje użytkownik.
TRIAL("Pole: filtr nie rusza treści ustawionej programem") {
    const auto one = std::make_shared<ui::Field>(10);

    one->allows([](char32_t which) { return which >= U'0' && which <= U'9'; });

    one->text("abc");

    same(one->text(), std::string{"abc"});
}

// Znak odsiany **nie idzie wyżej**. Inaczej „t" odrzucone w polu na cyfry naciskałoby w dialogu
// przycisk, który ma tę literę w klawiszach aktywnych.
TRIAL("Dialog: znak odsiany przez filtr nie naciska przycisku") {
    ui::Root top;

    int done = 0;

    const Asked asked = naming(top, &done);

    asked.field->allows([](char32_t which) { return which >= U'0' && which <= U'9'; });

    typed(top, "t");

    ought(asked.field->text().empty());
    same(done, 0);

    typed(top, "7");

    same(asked.field->text(), std::string{"7"});
}

// --- Kursor -----------------------------------------------------------------------------------
//
// Kursor wyszedł z kartki i należy do korzenia: widżet mówi, gdzie go chce, we własnych
// współrzędnych, a korzeń składa położenie przejściem w górę po prostokątach. Dopóki jechał
// razem z wklejeniem, przycinał się sam przez to, że węzeł spoza widoku w ogóle nie malował —
// teraz przycina go korzeń i to jest dokładnie to, co sprawdzają poniższe przypadki.

TRIAL("Kursor: zagnieżdżone pole składa położenie z prostokątów") {
    ui::Root top;

    const auto frame = std::make_shared<ui::Frame>();
    const auto column = std::make_shared<ui::Column>();
    const auto first = std::make_shared<ui::Field>(8, "ab");
    const auto second = std::make_shared<ui::Field>(8, "abc");

    column->children().add(first);
    column->children().add(second);
    frame->children().add(column);
    top.children().add(frame);

    second->focus();

    (void)top.render({{0, 0}, {12, 5}});

    // Ramka odsuwa treść o kratkę w każdą stronę, kolumna kładzie drugie pole w następnym
    // wierszu, a kursor stoi za trzecią literą.
    ought(top.caret().has_value());
    same(*top.caret(), (ui::Spot{4, 2}));
}

TRIAL("Kursor: pole zjechane poza okno przewijania nie zgłasza kursora") {
    ui::Root top;

    const auto scroll = std::make_shared<ui::Scroll>(ui::ScrollStyle{false});
    const auto column = std::make_shared<ui::Column>();

    for (int at = 0; at < 10; ++at) column->children().add(std::make_shared<ui::Field>(6, "ab"));

    const auto away = std::make_shared<ui::Field>(6, "ab");

    column->children().add(away);
    scroll->children().add(column);
    top.children().add(scroll);

    away->focus();

    (void)top.render({{0, 0}, {8, 3}});

    ought(!top.caret().has_value());

    // Przewinięcie do niego przywraca kursor — i to na krawędzi okna, a nie gdzieś dalej.
    scroll->shift({0, 8});

    (void)top.render({{0, 0}, {8, 3}});

    ought(top.caret().has_value());
    same(*top.caret(), (ui::Spot{2, 2}));
}

TRIAL("Kursor: pole bez ogniska go nie zgłasza, nawet gdy widać") {
    ui::Root top;

    const auto column = std::make_shared<ui::Column>();
    const auto one = std::make_shared<ui::Field>(6, "ab");
    const auto two = std::make_shared<ui::Field>(6, "cd");

    column->children().add(one);
    column->children().add(two);
    top.children().add(column);

    two->focus();

    (void)top.render({{0, 0}, {8, 2}});

    same(*top.caret(), (ui::Spot{2, 1}));

    one->focus();

    (void)top.render({{0, 0}, {8, 2}});

    same(*top.caret(), (ui::Spot{2, 0}));
}

// Rdzeń roli daje się podmienić, tak samo jak w przycisku, w pozycji menu i w polu legendy —
// pole tekstowe było jednym z ostatnich, które sięgały do skórki po napisie wpisanym na sztywno.
TRIAL("Field: rdzeń roli można podmienić") {
    ui::Root top;

    const auto one = std::make_shared<ui::Field>(6, "abc");

    same(one->role(), std::string{"talcite.input"});

    top.children().add(one);

    ui::Styles these;

    Style look{};

    look.ink = Color{false, 3, 3, 3};

    these["moje.pole"] = look;

    top.skin(ui::Skin{these});
    one->role("moje.pole");

    const View made = top.render({{0, 0}, {6, 1}});

    same(made.at({0, 0}).style.ink, (Color{false, 3, 3, 3}));
}
