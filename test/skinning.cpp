#include "proving.hpp"

#include <cstdint>
#include <memory>
#include <string>

#include "talcite/button.hpp"
#include "talcite/compatibilities.hpp"
#include "talcite/encoding.hpp"
#include "talcite/frame.hpp"
#include "talcite/line.hpp"
#include "talcite/list.hpp"
#include "talcite/listitem.hpp"
#include "talcite/root.hpp"
#include "talcite/span.hpp"
#include "talcite/skin.hpp"

using namespace talcite;

namespace {

// Węzeł, który pokazuje na zewnątrz to, co widżet dostaje w `paint`.
class Peek : public ui::Node {
public:
    [[nodiscard]] const ui::Skin& seen() const { return skin(); }
};

// Pojemnik, który układa dzieci w wiersze — tyle, ile trzeba, żeby lista i chętny liść stały obok.
class Hold : public ui::Node {
public:
    [[nodiscard]] int takes() const override { return ui::plenty; }

    [[nodiscard]] ui::Size wants(ui::Size room) const override { return room; }

protected:
    // Rysuje tylko pierwsze dziecko; reszta jest tu po to, żeby dało się jej oddać ognisko, i
    // dostaje prostokąt o zerowym rozmiarze, żeby niczego nie zamalowała.
    View paint(ui::Size given, ui::Area seen) override {
        View made{seen.size};

        bool first = true;

        for (const std::shared_ptr<ui::Node>& one : children()) {
            const ui::Area where{{0, 0}, first ? given : ui::Size{}};
            const ui::Area part = ui::Area::crossed(seen, where);

            made.within(one->render(where, part - where.at), part.at - seen.at);

            first = false;
        }

        return made;
    }
};

class Perch : public ui::Node {
public:
    [[nodiscard]] bool focusable() const override { return true; }
};

std::string read(const View& one, int y) {
    std::string made;

    for (int x = 0; x < one.size().wide; ++x) {
        const char32_t letter = one.at({x, y}).letter;

        if (letter != 0) talcite::spelled(letter, made);
    }

    while (!made.empty() && made.back() == ' ') made.pop_back();

    return made;
}

Style tinted(std::uint8_t red, std::uint8_t green, std::uint8_t blue) {
    Style made{};

    made.ink = Color{false, red, green, blue};

    return made;
}

}

TRIAL("Skin: wbudowana zna role i znaki") {
    const ui::Skin one;

    ought(one.get<Style>("talcite.list.focus").reverse);
    ought(one.get<Style>("talcite.input").under);
    ought(one.get<Style>("talcite.shade").faint);

    same(one.get<char32_t>("talcite.line.thin.across"), U'─');
    same(one.get<char32_t>("talcite.line.twin.across"), U'═');
    same(one.get<char32_t>("talcite.scroll.thumb"), U'█');
}

// **Odczyt nigdy nie rzuca**, bo woła się go z malowania. Nieznana rola daje barwy domyślne
// terminala, a nieznany znak — pytajnik.
TRIAL("Skin: nieznana rola daje pusty styl, nieznany znak pytajnik") {
    const ui::Skin one;

    same(one.get<Style>("czego.tu.nie.ma"), Style{});
    same(one.get<char32_t>("czego.tu.nie.ma"), U'?');
}

// Zero jest zabronione: w `View` znaczy dalszy ciąg znaku dwukolumnowego i składanie klatki takie
// kratki **pomija**, więc zero zrobiłoby dziurę w obrazie zamiast widocznego błędu.
TRIAL("Skin: nieznany znak nigdy nie jest zerem") {
    const ui::Skin one;

    ought(one.get<char32_t>("nie.ma.takiego") != 0);
    ought(ui::Skin::plain().get<char32_t>("nie.ma.takiego") != 0);
}

// To jest `Store(spare, held)`: wołający daje swoje, biblioteka dokłada resztę — tylko scalone raz,
// przy budowaniu, a nie przy każdym odczycie.
TRIAL("Skin: podane nadpisuje, reszta zostaje wbudowana") {
    const ui::Skin one{{{"talcite.list.focus", tinted(10, 20, 30)}}};

    same(one.get<Style>("talcite.list.focus"), tinted(10, 20, 30));

    // Nietknięte role dalej są takie, jak we wbudowanej.
    ought(one.get<Style>("talcite.input").under);
    ought(one.get<Style>("talcite.shade").faint);
    same(one.get<char32_t>("talcite.line.thin.across"), U'─');
}

TRIAL("Skin: plain zawsze oddaje wbudowaną, także gdy nadpisano") {
    const ui::Skin one{{{"talcite.list.focus", tinted(10, 20, 30)}}};

    same(one.get<Style>("talcite.list.focus"), tinted(10, 20, 30));
    ought(one.plain<Style>("talcite.list.focus").reverse);
    ought(one.plain<Style>("talcite.list.focus").ink.transparent);

    same(one.plain<char32_t>("talcite.line.thin.across"), U'─');
}

TRIAL("Skin: komplet ramki składa się z pojedynczych znaków") {
    const ui::Skin one;

    const ui::Lines thin = one.get<ui::Lines>("talcite.line.thin");

    same(thin.across, U'─');
    same(thin.upright, U'│');
    same(thin.upper_left, U'┌');
    same(thin.right_joint, U'┤');

    const ui::Lines twin = one.get<ui::Lines>("talcite.line.twin");

    same(twin.across, U'═');
    same(twin.lower_right, U'╝');

    // Nieznana grubość daje cienki — ten sam wybór, który robi `drawn` dla `LineStyle::none`.
    same(one.get<ui::Lines>("talcite.line.czegoś").across, U'─');
}

// Komplet jest **złożony**, a nie zapamiętany osobno: podmiana jednego znaku zmienia całą ósemkę.
// Poprawne z definicji, bo skórki nie da się zmienić po zbudowaniu.
TRIAL("Skin: podmiana znaku zmienia złożony komplet") {
    const ui::Skin one{{}, {{"talcite.line.thin.across", U'='}}};

    same(one.get<char32_t>("talcite.line.thin.across"), U'=');
    same(one.get<ui::Lines>("talcite.line.thin").across, U'=');

    // Reszta kompletu bez zmian.
    same(one.get<ui::Lines>("talcite.line.thin").upright, U'│');

    // I nie tknęło to grubszych.
    same(one.get<ui::Lines>("talcite.line.twin").across, U'═');
}

TRIAL("Skin: wbudowana to zawsze ten sam egzemplarz") {
    same(&ui::Skin::plain(), &ui::Skin::plain());
}

TRIAL("Skin: węzeł bez korzenia dostaje wbudowaną") {
    const Peek one;

    same(&one.seen(), &ui::Skin::plain());
    ought(one.seen().get<Style>("talcite.list.focus").reverse);
}

TRIAL("Skin: węzeł w drzewie dostaje skórkę korzenia") {
    ui::Root top;

    const auto one = std::make_shared<Peek>();
    const auto deeper = std::make_shared<Peek>();

    one->children().add(deeper);
    top.children().add(one);

    same(&one->seen(), &top.skin());
    same(&deeper->seen(), &top.skin());

    // Wbudowana to inny egzemplarz — korzeń ma własny.
    ought(&top.skin() != &ui::Skin::plain());
}

// **Podmiana całej palety jest całą zmianą wyglądu w locie**: nie ma powiadamiania ani obchodzenia
// drzewa, bo węzeł czyta ją dopiero wtedy, gdy maluje.
TRIAL("Skin: podmiana na korzeniu widoczna od razu w głębi") {
    ui::Root top;

    const auto one = std::make_shared<Peek>();
    const auto deeper = std::make_shared<Peek>();

    one->children().add(deeper);
    top.children().add(one);

    ought(deeper->seen().get<Style>("talcite.list.focus").reverse);

    top.skin(ui::Skin{{{"talcite.list.focus", tinted(1, 2, 3)}}});

    same(deeper->seen().get<Style>("talcite.list.focus"), tinted(1, 2, 3));

    // Reszta dalej z wbudowanej, bo nowa skórka też jest kompletna od urodzenia.
    ought(deeper->seen().get<Style>("talcite.input").under);
}

// Węzeł zdjęty z drzewa przestaje widzieć skórkę korzenia — i **nie zostaje bez niczego**.
TRIAL("Skin: zdjęty węzeł wraca do wbudowanej") {
    ui::Root top;

    const auto one = std::make_shared<Peek>();

    top.children().add(one);
    top.skin(ui::Skin{{{"talcite.input", tinted(9, 9, 9)}}});

    same(one->seen().get<Style>("talcite.input"), tinted(9, 9, 9));

    (void)top.children().remove(one.get());

    same(&one->seen(), &ui::Skin::plain());
    ought(one->seen().get<Style>("talcite.input").under);
}

// **Skórka nie jest zamknięta.** Aplikacja wymyśla własne role obok tych, które zna biblioteka,
// i czyta je tą samą drogą — bo klucz jest nazwą, a nie wyliczeniem, którego z zewnątrz nie da się
// rozszerzyć. Panel plików w programie plikowym będzie tego potrzebował na katalogi, pliki
// wykonywalne i zaznaczone.
TRIAL("Skin: aplikacja dokłada własne role obok wbudowanych") {
    ui::Root top;

    const auto one = std::make_shared<Peek>();

    top.children().add(one);

    top.skin(ui::Skin{{
        {"fork.panel.directory", tinted(1, 2, 3)},
        {"fork.panel.exec", tinted(4, 5, 6)},
    }});

    same(one->seen().get<Style>("fork.panel.directory"), tinted(1, 2, 3));
    same(one->seen().get<Style>("fork.panel.exec"), tinted(4, 5, 6));

    // Role biblioteki stoją obok, nietknięte.
    ought(one->seen().get<Style>("talcite.list.focus").reverse);
    ought(one->seen().get<Style>("talcite.input").under);
}

TRIAL("Skin: własne znaki dokłada się tak samo jak własne role") {
    const ui::Skin one{{}, {{"fork.panel.mark", U'*'}}};

    same(one.get<char32_t>("fork.panel.mark"), U'*');
    same(one.get<char32_t>("talcite.line.thin.across"), U'─');
}

// Rola, której nie ustawił nikt, daje barwy domyślne terminala — i **tego nie da się odróżnić** od
// roli ustawionej celowo na domyślne. Widżet spoza biblioteki, który chce mieć własny wygląd
// zapasowy, musi go dziś wsiać do skórki sam, przy jej budowaniu.
TRIAL("Skin: cudza rola bez wpisu daje domyślne terminala") {
    const ui::Skin one;

    same(one.get<Style>("fork.panel.directory"), Style{});
    same(one.plain<Style>("fork.panel.directory"), Style{});
}

// **Wierzchni dokłada, ale nie odbiera.** Po to jest scalanie: widżet spoza biblioteki trzyma swój
// wygląd zapasowy i nakłada na niego to, co powiedziała skórka — a gdy nie powiedziała nic, zostaje
// jego własny.
TRIAL("Style: scalanie dokłada kolor wierzchniego, a znaczniki sumuje") {
    Style base{};

    base.ink = Color{false, 1, 2, 3};
    base.under = true;

    Style over{};

    over.paper = Color{false, 9, 9, 9};
    over.reverse = true;

    const Style made = Style::merge(base, over);

    // Kolor spodni został, bo wierzchni go nie podał; tło przyszło z wierzchniego.
    same(made.ink, (Color{false, 1, 2, 3}));
    same(made.paper, (Color{false, 9, 9, 9}));

    ought(made.under);
    ought(made.reverse);
    ought(!made.bold);
}

TRIAL("Style: ustawiony kolor wierzchniego zasłania spodni") {
    Style base{};

    base.ink = Color{false, 1, 2, 3};

    Style over{};

    over.ink = Color{false, 7, 7, 7};

    same(Style::merge(base, over).ink, (Color{false, 7, 7, 7}));
}

// Pusty styl niczego nie zmienia — i to jest ten przypadek, dla którego scalanie powstało: rola,
// o której skórka nie słyszała, oddaje właśnie pusty styl.
TRIAL("Style: scalanie z pustym zostawia wygląd zapasowy") {
    Style base{};

    base.ink = Color{false, 1, 2, 3};
    base.bold = true;
    base.under = true;

    same(Style::merge(base, Style{}), base);

    // W drugą stronę tak samo: pusty spodni oddaje wierzchni bez zmian.
    same(Style::merge(Style{}, base), base);
}

// Cena reguły, wypowiedziana wprost: wierzchni **nie zgasi** znacznika spodniego, bo `Style` nie ma
// stanu „nie ustawiono" na znacznikach — ma go tylko kolor.
TRIAL("Style: wierzchni nie gasi znacznika spodniego") {
    Style base{};

    base.under = true;

    ought(Style::merge(base, Style{}).under);
}

// Tak wygląda zastosowanie: widżet spoza biblioteki ze swoim wyglądem zapasowym.
TRIAL("Skin: cudzy widżet nakłada skórkę na własny wygląd zapasowy") {
    Style mine{};

    mine.bold = true;

    const ui::Skin bare;

    // Skórka nie zna tej roli — zostaje wygląd własny.
    same(Style::merge(mine, bare.get<Style>("fork.panel.directory")), mine);

    const ui::Skin told{{{"fork.panel.directory", tinted(1, 2, 3)}}};

    const Style made = Style::merge(mine, told.get<Style>("fork.panel.directory"));

    same(made.ink, (Color{false, 1, 2, 3}));
    ought(made.bold);
}

// **Drugi zastępnik przestał być zastępnikiem.** Bieżący wiersz bez ogniska ma własną rolę, którą
// wbudowana skórka zostawia pustą — wygląda więc dokładnie jak dotąd, a pierwsza skórka z barwami
// wypełni ją bez zmiany jednej linijki kodu.
TRIAL("Skin: bieżący wiersz bez ogniska bierze rolę list.mark") {
    ui::Root top;

    const auto list = std::make_shared<ui::List>(20, 3);
    const auto first = std::make_shared<ui::ListItem>("jeden");
    const auto other = std::make_shared<ui::ListItem>("dwa");
    const auto perch = std::make_shared<Perch>();

    const auto nest = std::make_shared<Hold>();

    list->children().add(first);
    list->children().add(other);

    nest->children().add(list);
    nest->children().add(perch);
    top.children().add(nest);

    Style told{};

    told.ink = Color{false, 5, 6, 7};

    top.skin(ui::Skin{{{"talcite.list.mark", told}}});

    list->focus();

    (void)top.render({{0, 0}, {20, 3}});

    // Z ogniskiem bieżący wiersz jest w negatywie, a nie w roli „bez ogniska".
    same(list->which(), 0);
    ought(top.render({{0, 0}, {20, 3}}).at({0, 0}).style.reverse);

    // Po wyjściu ogniska zostaje sam znacznik — i wchodzi rola.
    perch->focus();

    const View made = top.render({{0, 0}, {20, 3}});

    same(list->which(), 0);
    ought(!made.at({0, 0}).style.reverse);
    same(made.at({0, 0}).style.ink.red, static_cast<std::uint8_t>(5));

    // Wiersz, który bieżący nie jest, roli nie dostaje.
    ought(list->which() != 1);
    ought(made.at({0, 1}).style.ink.transparent);
}

// **Znaki też przychodzą ze skórki.** Grubość dalej wybiera widżet — to jest wybór kształtu —
// a skórka mówi, jakimi znakami ta grubość jest rysowana.
TRIAL("Skin: znak ramki ze skórki idzie na ramkę") {
    ui::Root top;

    const auto frame = std::make_shared<ui::Frame>("kot", ui::FrameStyle{ui::LineStyle::twin});

    top.children().add(frame);

    same(read(top.render({{0, 0}, {11, 3}}), 0), std::string{"╔══ kot ══╗"});

    top.skin(ui::Skin{{}, {{"talcite.line.twin.across", U'*'}}});

    same(read(top.render({{0, 0}, {11, 3}}), 0), std::string{"╔** kot **╗"});
}

// **To jest spłacony dług zastępników ASCII**: terminal bez znaków rysunkowych nie potrzebuje
// osobnej gałęzi w kodzie, tylko drugiej skórki.
TRIAL("Skin: skórka ASCII rysuje ramkę bez znaków rysunkowych") {
    ui::Root top;

    const auto frame = std::make_shared<ui::Frame>("kot");

    top.children().add(frame);

    top.skin(ui::Skin{{},
                      {
                          {"talcite.line.thin.across", U'-'},
                          {"talcite.line.thin.upright", U'|'},
                          {"talcite.line.thin.upper.left", U'+'},
                          {"talcite.line.thin.upper.right", U'+'},
                          {"talcite.line.thin.lower.left", U'+'},
                          {"talcite.line.thin.lower.right", U'+'},
                      }});

    const View made = top.render({{0, 0}, {11, 3}});

    same(read(made, 0), std::string{"+-- kot --+"});
    same(read(made, 1), std::string{"|         |"});
    same(read(made, 2), std::string{"+---------+"});
}

TRIAL("Skin: wielokropek bierze się ze skórki") {
    ui::Root top;

    const auto one = std::make_shared<ui::Span>("dokumenty");

    one->elide(ui::ElideStyle::right);

    top.children().add(one);

    same(read(top.render({{0, 0}, {5, 1}}), 0), std::string{"doku…"});

    top.skin(ui::Skin{{}, {{"talcite.elide", U'~'}}});

    same(read(top.render({{0, 0}, {5, 1}}), 0), std::string{"doku~"});
}

TRIAL("Skin: strzałki i suwak przewijaka biorą znaki ze skórki") {
    ui::Root top;

    const auto list = std::make_shared<ui::List>(ui::plenty, ui::plenty, ui::ListStyle{true});

    for (int which = 0; which < 10; ++which)
        list->children().add(std::make_shared<ui::ListItem>("wiersz"));

    top.children().add(list);

    View made = top.render({{0, 0}, {8, 4}});

    same(made.at({7, 0}).letter, U'▲');
    same(made.at({7, 3}).letter, U'▼');

    top.skin(ui::Skin{{},
                      {
                          {"talcite.scroll.up", U'^'},
                          {"talcite.scroll.down", U'v'},
                          {"talcite.scroll.thumb", U'#'},
                          {"talcite.scroll.track", U'.'},
                      }});

    made = top.render({{0, 0}, {8, 4}});

    same(made.at({7, 0}).letter, U'^');
    same(made.at({7, 3}).letter, U'v');
    same(made.at({7, 1}).letter, U'#');
    same(made.at({7, 2}).letter, U'.');
}

// --- Rdzeń roli zadawany widżetowi -------------------------------------------------------------
//
// Jedna rola dla wszystkich napisów w programie nie umie być naraz czerwona i granatowa, a napis
// w pudełku błędu ma być czerwony. Kto buduje takie pudełko, ten wie, co w nim stoi — więc rdzeń
// nazwy roli zadaje się widżetowi z zewnątrz, a końcówki na stan dokłada sam widżet.

namespace {

Style inked(std::uint8_t red, std::uint8_t green, std::uint8_t blue) {
    Style made{};

    made.ink = Color{false, red, green, blue};

    return made;
}

}

TRIAL("Rola: napis pyta o rdzeń, który mu zadano") {
    ui::Root top;

    top.skin(ui::Skin{{{"talcite.normal", inked(1, 1, 1)}, {"moja.rola", inked(9, 9, 9)}}});

    const auto one = std::make_shared<ui::Span>("kot");

    top.children().add(one);

    same(one->role(), std::string{"talcite.normal"});
    same(top.render({{0, 0}, {5, 1}}).at({0, 0}).style.ink, (Color{false, 1, 1, 1}));

    one->role("moja.rola");

    same(top.render({{0, 0}, {5, 1}}).at({0, 0}).style.ink, (Color{false, 9, 9, 9}));
}

TRIAL("Rola: ramka pyta o rdzeń, który jej zadano") {
    ui::Root top;

    Style mine{};

    mine.paper = Color{false, 7, 7, 7};

    top.skin(ui::Skin{{{"talcite.frame", inked(1, 1, 1)}, {"moja.ramka", mine}}});

    const auto one = std::make_shared<ui::Frame>();

    top.children().add(one);

    one->role("moja.ramka");

    const View made = top.render({{0, 0}, {6, 3}});

    // Kreska i tło biorą się z tej samej roli.
    same(made.at({0, 0}).style.paper, (Color{false, 7, 7, 7}));
    same(made.at({2, 1}).style.paper, (Color{false, 7, 7, 7}));
}

TRIAL("Rola: przycisk dokłada końcówkę na stan") {
    ui::Root top;

    ui::Styles these;

    these["moj.guzik"] = inked(1, 1, 1);
    these["moj.guzik.focus"] = inked(2, 2, 2);
    these["moj.guzik.off"] = inked(3, 3, 3);

    top.skin(ui::Skin{these});

    const auto one = std::make_shared<ui::Button>("Tak");

    top.children().add(one);

    same(one->role(), std::string{"talcite.button"});

    one->role("moj.guzik");

    same(top.render({{0, 0}, {9, 1}}).at({0, 0}).style.ink, (Color{false, 1, 1, 1}));

    one->focus();

    same(top.render({{0, 0}, {9, 1}}).at({0, 0}).style.ink, (Color{false, 2, 2, 2}));

    one->able(false);

    same(top.render({{0, 0}, {9, 1}}).at({0, 0}).style.ink, (Color{false, 3, 3, 3}));
}

TRIAL("Rola: litera aktywna też idzie za rdzeniem") {
    ui::Root top;

    ui::Styles these;

    Style hot{};

    hot.reverse = true;

    these["moj.guzik"] = inked(1, 1, 1);
    these["moj.guzik.key"] = hot;

    top.skin(ui::Skin{these});

    const auto one = std::make_shared<ui::Button>("Tak");

    one->keys().add(U'T');
    one->role("moj.guzik");

    top.children().add(one);

    const View made = top.render({{0, 0}, {9, 1}});

    // `[ Tak ]` — litera T stoi na trzeciej kratce.
    same(made.at({2, 0}).letter, U'T');
    ought(made.at({2, 0}).style.reverse);
    ought(!made.at({3, 0}).style.reverse);
}

// Rodzina komunikatu i rodzina błędu muszą być w skórce wbudowanej, inaczej przycisk z ogniskiem
// w komunikacie straciłby podświetlenie: pytałby o rolę, o której nikt nie słyszał.
TRIAL("Skin: wbudowana zna rodzinę komunikatu i błędu") {
    const ui::Skin one;

    for (const std::string& stem : {std::string{"talcite.message"}, std::string{"talcite.error"}}) {
        ought(one.get<Style>(stem + ".button.focus").reverse);
        ought(one.get<Style>(stem + ".button.key").under);
        ought(one.get<Style>(stem + ".button.off").faint);

        // Reszta rodziny jest pusta z rozmysłem: bez barw pudełko błędu niczym się nie różni od
        // zwykłego okna, tak samo jak w mono-skórce Midnight Commandera.
        same(one.get<Style>(stem), Style{});
        same(one.get<Style>(stem + ".title"), Style{});
        same(one.get<Style>(stem + ".text"), Style{});
        same(one.get<Style>(stem + ".box"), Style{});
    }
}


// --- Warianty na poziomy terminala ----------------------------------------------------------
//
// Skórka niesie wartość roli na każdy poziom barwności i znaków; który obowiązuje, mówi jej korzeń
// z faktów o terminalu. Wariantu brak — wbudowana, bez przybliżania.

namespace {

ui::Shades shaded() {
    ui::Shades made;

    made.ansi = tinted(1, 1, 1);
    made.rgb = tinted(3, 3, 3);

    return made;
}

}

TRIAL("Skin: jedna wartość obowiązuje na każdym poziomie") {
    ui::Skin one{{{"talcite.list.focus", tinted(10, 20, 30)}}};

    for (const Colors level : {Colors::mono, Colors::ansi, Colors::palette, Colors::rgb}) {
        one.colors(level);

        same(one.get<Style>("talcite.list.focus"), tinted(10, 20, 30));
    }
}

TRIAL("Skin: wariant to najwyższy nieprzekraczający poziomu") {
    ui::Skin one{{{"talcite.list.focus", shaded()}}};

    one.colors(Colors::ansi);
    same(one.get<Style>("talcite.list.focus"), tinted(1, 1, 1));

    // Na `palette` nie ma własnego wariantu — obowiązuje `ansi`, nie `rgb`: wyżej się nie sięga.
    one.colors(Colors::palette);
    same(one.get<Style>("talcite.list.focus"), tinted(1, 1, 1));

    one.colors(Colors::rgb);
    same(one.get<Style>("talcite.list.focus"), tinted(3, 3, 3));
}

TRIAL("Skin: bez wariantu na poziom rola wraca do wbudowanej, nie do przybliżenia") {
    ui::Skin one{{{"talcite.list.focus", shaded()}}};

    // `mono` nie ma nic poniżej siebie, a skórka zaczyna od `ansi` — więc wbudowana: odwrócenie.
    one.colors(Colors::mono);

    same(one.get<Style>("talcite.list.focus"), ui::Skin::plain().get<Style>("talcite.list.focus"));
    ought(one.get<Style>("talcite.list.focus").reverse);
    ought(one.get<Style>("talcite.list.focus").ink.transparent);
}

TRIAL("Skin: rola spoza wbudowanych bez wariantu daje pusty styl") {
    ui::Skin one{{{"app.thing", shaded()}}};

    one.colors(Colors::mono);

    same(one.get<Style>("app.thing"), Style{});
}

TRIAL("Skin: znak ma wersję ASCII i unicode, terminal bez unicode bierze tylko ASCII") {
    ui::Signs sign;

    sign.ascii = U'-';
    sign.unicode = U'─';

    ui::Skin one{{}, {{"talcite.line.thin.across", sign}}};

    same(one.get<char32_t>("talcite.line.thin.across"), U'-');
    same(one.get<ui::Lines>("talcite.line.thin").across, U'-');

    one.unicode(true);

    same(one.get<char32_t>("talcite.line.thin.across"), U'─');
    same(one.get<ui::Lines>("talcite.line.thin").across, U'─');
}

TRIAL("Skin: znak z samą wersją unicode na terminalu bez unicode wraca do wbudowanego") {
    ui::Signs sign;

    sign.unicode = U'=';

    ui::Skin one{{}, {{"talcite.line.thin.across", sign}}};

    same(one.get<char32_t>("talcite.line.thin.across"), U'─');

    one.unicode(true);

    same(one.get<char32_t>("talcite.line.thin.across"), U'=');
}

TRIAL("Root: fakty o terminalu przechodzą na każdą kolejną skórkę") {
    ui::Root top;

    top.colors(Colors::rgb);
    top.unicode(true);

    top.skin(ui::Skin{{{"talcite.list.focus", shaded()}}});

    same(static_cast<int>(top.skin().colors()), static_cast<int>(Colors::rgb));
    ought(top.skin().unicode());
    same(top.skin().get<Style>("talcite.list.focus"), tinted(3, 3, 3));

    top.colors(Colors::ansi);

    same(top.skin().get<Style>("talcite.list.focus"), tinted(1, 1, 1));
}

TRIAL("Root: bez faktów skórka stoi na mono i ASCII") {
    ui::Root top;

    same(static_cast<int>(top.skin().colors()), static_cast<int>(Colors::mono));
    ought(!top.skin().unicode());
}
