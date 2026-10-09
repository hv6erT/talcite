#include "proving.hpp"

#include <memory>
#include <string>

#include "talcite/encoding.hpp"
#include "talcite/column.hpp"
#include "talcite/frame.hpp"
#include "talcite/root.hpp"
#include "talcite/skin.hpp"
#include "talcite/line.hpp"
#include "talcite/span.hpp"
#include "talcite/scroll.hpp"

using namespace talcite;

namespace {

class Box : public ui::Node {
public:
    Box(ui::Size wants, char32_t what) : wants_(wants), what_(what) {}

    [[nodiscard]] ui::Size wants(ui::Size) const override { return wants_; }

protected:
    View paint(ui::Size given, ui::Area seen) override {
        View made{seen.size};

        for (int y = 0; y < given.high; ++y)
            for (int x = 0; x < given.wide; ++x)
                made.put({x - seen.at.x, y - seen.at.y}, what_, Style{});

        return made;
    }

private:
    ui::Size wants_;
    char32_t what_ = U'?';
};

std::string read(const View& one, int y) {
    std::string made;

    for (int x = 0; x < one.size().wide; ++x) {
        const char32_t letter = one.at({x, y}).letter;

        if (letter != 0) talcite::utf8::encode(letter, made);
    }

    while (!made.empty() && made.back() == ' ') made.pop_back();

    return made;
}

}

TRIAL("Frame: narożniki, boki i wnętrze") {
    ui::Frame one;

    one.children().add(std::make_shared<Box>(ui::Size{4, 2}, U'x'));

    const View made = one.render({{0, 0}, {6, 4}});

    same(read(made, 0), std::string{"┌────┐"});
    same(read(made, 1), std::string{"│xxxx│"});
    same(read(made, 2), std::string{"│xxxx│"});
    same(read(made, 3), std::string{"└────┘"});
}

TRIAL("Frame: podwójna i gruba linia") {
    ui::Frame one;

    one.border(ui::Border::twin);

    same(read(one.render({{0, 0}, {4, 2}}), 0), std::string{"╔══╗"});

    one.border(ui::Border::thick);

    same(read(one.render({{0, 0}, {4, 2}}), 0), std::string{"┏━━┓"});
}

TRIAL("Frame: bez linii nie ma ramki i dziecko dostaje wszystko") {
    ui::Frame one{"tytuł", ui::FrameShape{ui::Border::none}};

    const auto inner = std::make_shared<Box>(ui::Size{4, 2}, U'x');

    one.children().add(inner);

    const View made = one.render({{0, 0}, {6, 4}});

    same(inner->area().size.wide, 6);
    same(inner->area().size.high, 4);
    same(read(made, 0), std::string{"xxxxxx"});
}

TRIAL("Frame: ramka zabiera dziecku po kratce z każdej strony") {
    ui::Frame one;

    const auto inner = std::make_shared<Box>(ui::Size{10, 10}, U'x');

    one.children().add(inner);

    (void)one.render({{0, 0}, {8, 5}});

    same(inner->area().at.x, 1);
    same(inner->area().at.y, 1);
    same(inner->area().size.wide, 6);
    same(inner->area().size.high, 3);
}

TRIAL("Frame: chce tyle, co dziecko, plus dwa w obu wymiarach") {
    ui::Frame one;

    one.children().add(std::make_shared<Box>(ui::Size{4, 2}, U'x'));

    const ui::Size wants = one.wants({80, 25});

    same(wants.wide, 6);
    same(wants.high, 4);
}

TRIAL("Frame: nie chce więcej, niż dostał od rodzica") {
    ui::Frame one;

    one.children().add(std::make_shared<Box>(ui::Size{40, 40}, U'x'));

    const ui::Size wants = one.wants({8, 5});

    same(wants.wide, 8);
    same(wants.high, 5);
}

TRIAL("Frame: tytuł stoi w górnej krawędzi, wyśrodkowany") {
    ui::Frame one{"kot"};

    const View made = one.render({{0, 0}, {11, 3}});

    same(read(made, 0), std::string{"┌── kot ──┐"});
}

TRIAL("Frame: tytuł do lewej i do prawej") {
    ui::Frame one{"kot"};

    one.justify(ui::Justify::left);

    same(read(one.render({{0, 0}, {11, 3}}), 0), std::string{"┌ kot ────┐"});

    one.justify(ui::Justify::right);

    same(read(one.render({{0, 0}, {11, 3}}), 0), std::string{"┌──── kot ┐"});
}

// Tytuł i stopka mają osobne wyrównania: w MC ścieżka stoi na środku górnej krawędzi, a wiersz
// stanu do lewej — jedno `justify` na obie nie dawało się tak ustawić.
TRIAL("Frame: stopka ma własne wyrównanie, niezależne od tytułu") {
    ui::Frame one{"kot"};

    one.footer("pies");

    same(one.footing(), ui::Justify::center);

    one.footing(ui::Justify::left);

    const View made = one.render({{0, 0}, {11, 3}});

    same(read(made, 0), std::string{"┌── kot ──┐"});
    same(read(made, 2), std::string{"└ pies ───┘"});
    same(one.justify(), ui::Justify::center);

    one.justify(ui::Justify::right);

    same(read(one.render({{0, 0}, {11, 3}}), 2), std::string{"└ pies ───┘"});
}

TRIAL("Frame: za długi tytuł skraca się wielokropkiem") {
    ui::Frame one{"bardzo długi tytuł"};

    const View made = one.render({{0, 0}, {10, 3}});

    same(read(made, 0), std::string{"┌ bardzo…┐"});
}

TRIAL("Frame: dolny napis stoi w dolnej krawędzi") {
    ui::Frame one{"kot"};

    one.footer("3 pliki");

    const View made = one.render({{0, 0}, {13, 3}});

    same(read(made, 0), std::string{"┌─── kot ───┐"});
    same(read(made, 2), std::string{"└─ 3 pliki ─┘"});
}

TRIAL("Frame: napisy wymieniają się przez gettery i settery") {
    ui::Frame one{"kot"};

    same(one.title(), std::string{"kot"});
    same(one.footer(), std::string{""});

    one.title("pies");
    one.footer("dwa");

    same(one.title(), std::string{"pies"});
    same(one.footer(), std::string{"dwa"});
    same(one.border(), ui::Border::thin);
}

// Linie i tytuł mają **osobne role**, więc skórka rozstrzyga o obu z osobna — i to jest różnica
// wobec dawnego jednego koloru na całą ramkę.
TRIAL("Frame: linie i napis biorą kolor z osobnych ról") {
    ui::Root top;

    const auto one = std::make_shared<ui::Frame>("kot");

    top.children().add(one);

    Style edge{};
    Style told{};

    edge.foreground = Color{false, 10, 20, 30};
    told.foreground = Color{false, 40, 50, 60};

    // Tytuł idzie za rdzeniem ramki: `talcite.frame.title`, nie wspólne `talcite.title`.
    top.skin(ui::Skin{{{"talcite.frame", edge}, {"talcite.frame.title", told}}});

    const View made = top.render({{0, 0}, {11, 3}});

    same(made.at({0, 0}).style.foreground.red, static_cast<std::uint8_t>(10));
    same(made.at({4, 0}).style.foreground.red, static_cast<std::uint8_t>(40));

    // Zmieniony rdzeń przenosi ze sobą tytuł — to jest cały powód tej zmiany.
    Style other{};

    other.foreground = Color{false, 70, 80, 90};

    one->role("app.panel");
    top.skin(ui::Skin{{{"app.panel.title", other}}});

    same(top.render({{0, 0}, {11, 3}}).at({4, 0}).style.foreground.red, static_cast<std::uint8_t>(70));
}

// Stopka jest w barwie linii, nie tytułu — jak wiersz stanu pod listą plików w MC, który idzie
// zwykłą barwą panelu, choć tytuł czynnego panelu jest odwrotny.
TRIAL("Frame: dolny napis bierze kolor linii, nie tytułu") {
    ui::Root top;

    const auto one = std::make_shared<ui::Frame>("kot");

    one->footer("pies");

    top.children().add(one);

    Style edge{};
    Style told{};

    edge.foreground = Color{false, 10, 20, 30};
    told.foreground = Color{false, 40, 50, 60};

    top.skin(ui::Skin{{{"talcite.frame", edge}, {"talcite.frame.title", told}}});

    const View made = top.render({{0, 0}, {11, 3}});

    same(read(made, 2), std::string{"└─ pies ──┘"});
    same(made.at({4, 0}).style.foreground.red, static_cast<std::uint8_t>(40));
    same(made.at({4, 2}).style.foreground.red, static_cast<std::uint8_t>(10));
}

TRIAL("Frame: ciasny prostokąt niczego nie wywraca") {
    ui::Frame one{"kot"};

    one.children().add(std::make_shared<Box>(ui::Size{4, 4}, U'x'));

    const View first = one.render({{0, 0}, {1, 1}});

    same(first.size().wide, 1);

    const View second = one.render({{0, 0}, {2, 2}});

    same(read(second, 0), std::string{"┌┐"});
    same(read(second, 1), std::string{"└┘"});
}

TRIAL("Frame: pusta ramka rysuje się bez dziecka") {
    ui::Frame one{"kot"};

    const View made = one.render({{0, 0}, {7, 3}});

    same(read(made, 1), std::string{"│     │"});
    same(one.wants({80, 25}).wide, 2);
}

TRIAL("Frame: bez górnej krawędzi linia idzie przez całą szerokość") {
    ui::Frame one{"kot"};

    one.top(false);

    const View made = one.render({{0, 0}, {6, 3}});

    same(read(made, 0), std::string{"│    │"});
    same(read(made, 2), std::string{"└────┘"});
}

TRIAL("Frame: bez górnej krawędzi nie ma tytułu") {
    ui::Frame one{"kot"};

    one.footer("stopka");
    one.top(false);

    const View made = one.render({{0, 0}, {12, 3}});

    ought(read(made, 0).find("kot") == std::string::npos);
    ought(read(made, 2).find("stopka") != std::string::npos);
}

TRIAL("Frame: bez dolnej krawędzi nie ma dolnego napisu") {
    ui::Frame one{"kot"};

    one.footer("stopka");
    one.bottom(false);

    const View made = one.render({{0, 0}, {12, 3}});

    ought(read(made, 0).find("kot") != std::string::npos);
    ought(read(made, 2).find("stopka") == std::string::npos);
}

TRIAL("Frame: bez boku znika kolumna i narożniki po tej stronie") {
    ui::Frame one;

    one.left(false);

    const View made = one.render({{0, 0}, {5, 3}});

    same(read(made, 0), std::string{"────┐"});
    same(read(made, 1), std::string{"    │"});
    same(read(made, 2), std::string{"────┘"});
}

TRIAL("Frame: wyłączona krawędź nie zabiera treści miejsca") {
    ui::Frame one;

    const auto inner = std::make_shared<Box>(ui::Size{10, 10}, U'x');

    one.children().add(inner);

    one.top(false);
    one.right(false);

    (void)one.render({{0, 0}, {8, 5}});

    same(inner->area().at.x, 1);
    same(inner->area().at.y, 0);
    same(inner->area().size.wide, 7);
    same(inner->area().size.high, 4);

    const ui::Size wants = one.wants({80, 25});

    same(wants.wide, 11);
    same(wants.high, 11);
}

TRIAL("Frame: sama górna krawędź to zwykła kreska z tytułem") {
    ui::Frame one{"kot"};

    one.bottom(false);
    one.left(false);
    one.right(false);

    const auto inner = std::make_shared<Box>(ui::Size{9, 2}, U'x');

    one.children().add(inner);

    const View made = one.render({{0, 0}, {9, 3}});

    same(read(made, 0), std::string{"── kot ──"});
    same(read(made, 1), std::string{"xxxxxxxxx"});
    same(inner->area().at.y, 1);
}

TRIAL("Frame: wszystkie krawędzie wyłączone to to samo, co brak linii") {
    ui::Frame one{"kot"};

    one.top(false);
    one.bottom(false);
    one.left(false);
    one.right(false);

    const auto inner = std::make_shared<Box>(ui::Size{4, 2}, U'x');

    one.children().add(inner);

    const View made = one.render({{0, 0}, {4, 2}});

    same(read(made, 0), std::string{"xxxx"});
    same(inner->area().size.wide, 4);
}

TRIAL("Frame: przewinięta ramka rysuje właściwy kawałek") {
    ui::Scroll scroll{ui::Axis::vertical};

    const auto frame = std::make_shared<ui::Frame>("kot");

    frame->children().add(std::make_shared<Box>(ui::Size{4, 6}, U'x'));

    scroll.children().add(frame);

    scroll.shift({0, 2});

    const View made = scroll.render({{0, 0}, {6, 3}});

    same(read(made, 0), std::string{"│xxxx│"});
    same(read(made, 2), std::string{"│xxxx│"});
}

TRIAL("Frame: ramka w ramce") {
    ui::Frame outer{"z zewnątrz"};

    const auto inner = std::make_shared<ui::Frame>();

    inner->border(ui::Border::twin);
    inner->children().add(std::make_shared<Box>(ui::Size{2, 1}, U'x'));

    outer.children().add(inner);

    const View made = outer.render({{0, 0}, {8, 5}});

    same(read(made, 1), std::string{"│╔════╗│"});
    same(read(made, 2), std::string{"│║xxxx║│"});
    same(read(made, 3), std::string{"│╚════╝│"});
}

TRIAL("Line: sama linia przez całą daną szerokość") {
    ui::Line one;

    const View made = one.render({{0, 0}, {8, 1}});

    same(read(made, 0), std::string{"────────"});
}

// Rozdzielnik **nie rozpycha rodzica**: prosi o szerokość samego napisu, a bez napisu o zero.
// Rozciąga go kolumna, bo każdemu dziecku daje całą swoją szerokość.
TRIAL("Line: prosi o napis, a bez napisu o nic") {
    ui::Line bare;

    same(bare.wants({20, 3}), (ui::Size{0, 1}));

    ui::Line said{"Opcje"};

    // Pięć liter i po spacji z każdej strony.
    same(said.wants({20, 3}), (ui::Size{7, 1}));
    same(said.wants({4, 3}), (ui::Size{4, 1}));

    // Rozdzielnik, który się zawija, to nie rozdzielnik.
    same(said.wants({20, 0}), (ui::Size{7, 0}));
}

TRIAL("Line: napis stoi w linii, domyślnie na środku") {
    ui::Line one{"Opcje"};

    View made = one.render({{0, 0}, {13, 1}});

    same(read(made, 0), std::string{"─── Opcje ───"});

    one.justify(ui::Justify::left);

    made = one.render({{0, 0}, {13, 1}});

    same(read(made, 0), std::string{" Opcje ──────"});

    one.justify(ui::Justify::right);

    made = one.render({{0, 0}, {13, 1}});

    same(read(made, 0), std::string{"────── Opcje"});
}

TRIAL("Line: za długi napis przycina się wielokropkiem") {
    ui::Line one{"Ustawienia panelu"};

    const View made = one.render({{0, 0}, {8, 1}});

    same(read(made, 0), std::string{" Ustawi…"});
}

TRIAL("Line: grubość bierze się ze stylu, a brak linii zostawia sam napis") {
    ui::Line twin{"Opcje", ui::Border::twin};

    same(read(twin.render({{0, 0}, {13, 1}}), 0), std::string{"═══ Opcje ═══"});

    ui::Line thick{std::string{}, ui::Border::thick};

    same(read(thick.render({{0, 0}, {4, 1}}), 0), std::string{"━━━━"});

    ui::Line none{"Opcje", ui::Border::none};

    same(read(none.render({{0, 0}, {13, 1}}), 0), std::string{"    Opcje"});
}

TRIAL("Line: nie przyjmuje dzieci") {
    ui::Line one;

    same(one.capacity(), 0);
}

// Rozdzielnik jest kawałkiem ramki: bez roli wygląda jak `talcite.frame`, z rolą — jak ta rola,
// z napisem w `.title`. Tak kreska nad wierszem stanu panelu idzie za panelem czynnym i biernym.
TRIAL("Line: kreska i napis biorą kolor z roli, domyślnie z talcite.frame") {
    ui::Root top;

    const auto one = std::make_shared<ui::Line>("kot");

    top.children().add(one);

    same(one->role(), std::string{"talcite.frame"});

    Style edge{};
    Style told{};
    Style other{};

    edge.foreground = Color{false, 10, 20, 30};
    told.foreground = Color{false, 40, 50, 60};
    other.foreground = Color{false, 70, 80, 90};

    top.skin(ui::Skin{{{"talcite.frame", edge},
                       {"talcite.frame.title", told},
                       {"app.panel", other},
                       {"app.panel.title", other}}});

    const View first = top.render({{0, 0}, {9, 1}});

    same(read(first, 0), std::string{"── kot ──"});
    same(first.at({0, 0}).style.foreground.red, static_cast<std::uint8_t>(10));
    same(first.at({3, 0}).style.foreground.red, static_cast<std::uint8_t>(40));

    one->role("app.panel");

    const View second = top.render({{0, 0}, {9, 1}});

    same(second.at({0, 0}).style.foreground.red, static_cast<std::uint8_t>(70));
    same(second.at({3, 0}).style.foreground.red, static_cast<std::uint8_t>(70));
}

// **Rozdzielnik dochodzi do ramki.** Dziecko nie maluje poza swoim prostokątem, więc złączkę
// stawia ramka — poznając rozdzielnik po złożonym obrazie, a nie po typie węzła. Dzięki temu
// działa to przez dowolne zagnieżdżenie.
TRIAL("Frame: rozdzielnik w środku dostaje złączki") {
    ui::Frame frame;

    const auto column = std::make_shared<ui::Column>();

    column->children().add(std::make_shared<Box>(ui::Size{6, 1}, U'x'));
    column->children().add(std::make_shared<ui::Line>());
    column->children().add(std::make_shared<Box>(ui::Size{6, 1}, U'y'));

    frame.children().add(column);

    const View made = frame.render({{0, 0}, {8, 5}});

    same(read(made, 0), std::string{"┌──────┐"});
    same(read(made, 1), std::string{"│xxxxxx│"});
    same(read(made, 2), std::string{"├──────┤"});
    same(read(made, 3), std::string{"│yyyyyy│"});
    same(read(made, 4), std::string{"└──────┘"});
}

TRIAL("Frame: złączka wchodzi tylko przy zgodnej grubości") {
    ui::Frame frame{{}, ui::FrameShape{ui::Border::twin}};

    const auto column = std::make_shared<ui::Column>();

    column->children().add(std::make_shared<ui::Line>());
    column->children().add(std::make_shared<ui::Line>(std::string{}, ui::Border::twin));

    frame.children().add(column);

    const View made = frame.render({{0, 0}, {8, 4}});

    // Cienka kreska w grubej ramce nie łączy się — bo i nie ma z czym.
    same(read(made, 1), std::string{"║──────║"});
    same(read(made, 2), std::string{"╠══════╣"});
}

TRIAL("Frame: złączki dostaje tylko treść dotykająca obu boków") {
    ui::Frame frame;

    const auto column = std::make_shared<ui::Column>();

    column->children().add(std::make_shared<ui::Line>("Opcje"));
    column->children().add(std::make_shared<Box>(ui::Size{12, 1}, U'x'));

    frame.children().add(column);

    const View made = frame.render({{0, 0}, {14, 4}});

    // Napis nie sięga boków, ale kreska sięga — więc złączki są.
    same(read(made, 1), std::string{"├── Opcje ───┤"});

    // Zwykła treść nie.
    same(read(made, 2), std::string{"│xxxxxxxxxxxx│"});
}

// Napis szerszy niż wnętrze zjada kreskę na brzegach — i wtedy złączek nie ma, bo nie ma czego
// łączyć. To ta sama reguła, tylko widziana od drugiej strony.
TRIAL("Frame: rozdzielnik wypełniony napisem złączek nie dostaje") {
    ui::Frame frame;

    const auto column = std::make_shared<ui::Column>();

    column->children().add(std::make_shared<ui::Line>("Ustawienia"));

    frame.children().add(column);

    const View made = frame.render({{0, 0}, {8, 3}});

    same(read(made, 1), std::string{"│ Usta…│"});
}

// --- Tło ramki --------------------------------------------------------------------------------
//
// Wnętrze zostawało dotąd spacjami w stylu domyślnym, więc pudełko w barwnej skórce było
// przezroczyste wszędzie tam, gdzie dziecko go nie zakryło. Żadne okno w Midnight Commanderze
// tak nie wygląda: odstęp przy krawędzi, prześwit obok krótszego napisu i wiersz pod ostatnim
// dzieckiem są w kolorze okna.

namespace {

ui::Skin painted() {
    Style frame{};
    Style normal{};

    frame.background = Color{false, 0, 0, 160};
    frame.foreground = Color{false, 200, 200, 200};

    normal.foreground = Color{false, 255, 255, 0};

    return ui::Skin{{{"talcite.frame", frame}, {"talcite.normal", normal}}};
}

}

TRIAL("Frame: wnętrze bierze barwę roli ramki, także tam, gdzie nic nie stoi") {
    ui::Root top;

    top.skin(painted());

    const auto frame = std::make_shared<ui::Frame>("Tytuł");

    frame->children().add(std::make_shared<ui::Span>("kot"));

    top.children().add(frame);

    const View made = top.render({{0, 0}, {12, 5}});

    const Color background = Color{false, 0, 0, 160};

    // Krawędź, prześwit obok krótszego napisu i wiersz pod ostatnim dzieckiem — w tle okna.
    same(made.at({0, 0}).style.background, background);
    same(made.at({8, 1}).style.background, background);
    same(made.at({5, 3}).style.background, background);

    // **Kratka ma jeden styl, nie dwa.** Pod literą napisu stoi rola napisu, a nie tło ramki —
    // tło jest tym, co widać tam, gdzie nikt nic nie namalował, a nie warstwą pod spodem.
    // W barwnej skórce to rola napisu musi podać własne tło, i tak właśnie robi skórka z dema.
    same(read(made, 1), std::string{"│kot       │"});
    same(made.at({1, 1}).letter, U'k');
    same(made.at({1, 1}).style.foreground, (Color{false, 255, 255, 0}));
    ought(made.at({1, 1}).style.background.transparent);
}

TRIAL("Frame: bez krawędzi tło i tak jest") {
    ui::Root top;

    top.skin(painted());

    const auto frame = std::make_shared<ui::Frame>(std::string{},
                                                   ui::FrameShape{ui::Border::none});

    top.children().add(frame);

    const View made = top.render({{0, 0}, {6, 2}});

    same(made.at({0, 0}).style.background, (Color{false, 0, 0, 160}));
    same(made.at({5, 1}).style.background, (Color{false, 0, 0, 160}));
}

// Skórka wbudowana nie ma barw, więc wypełnienie zapisuje dokładnie to, co i tak stało na kartce.
TRIAL("Frame: pod wbudowaną skórką tło niczego nie zmienia") {
    ui::Frame frame;

    frame.children().add(std::make_shared<ui::Span>("kot"));

    const View made = frame.render({{0, 0}, {9, 4}});

    same(made.at({4, 2}), (Cell{U' ', Style{}, Shown::own}));
    same(made.at({7, 1}), (Cell{U' ', Style{}, Shown::own}));
}

// --- Przezroczystość pojemników ---------------------------------------------------------------
//
// Pojemnik nie ma własnego wyglądu: układa dzieci i nic poza tym. Zamalowywanie przydzielonego
// prostokąta własnymi spacjami sprawiało, że tło rodzica ginęło wszędzie tam, gdzie dziecko go
// nie zakryło — a to jest większość okna.

TRIAL("Frame: tło przechodzi przez kolumnę i wiersz do prześwitów") {
    ui::Root top;

    top.skin(painted());

    const auto frame = std::make_shared<ui::Frame>();
    const auto column = std::make_shared<ui::Column>();

    column->children().add(std::make_shared<ui::Span>("ab"));
    column->children().add(std::make_shared<ui::Span>("cd"));

    frame->children().add(column);
    top.children().add(frame);

    const View made = top.render({{0, 0}, {10, 6}});

    const Color background = Color{false, 0, 0, 160};

    // Obok krótszego napisu, pod ostatnim dzieckiem i w narożniku wnętrza.
    same(made.at({5, 1}).style.background, background);
    same(made.at({3, 3}).style.background, background);
    same(made.at({8, 4}).style.background, background);
}

// Pusty węzeł **nie jest odstępem**: bez dzieci chce zera na zero, więc kolumna nie daje mu ani
// wiersza. Odstęp wymaga węzła, który poprosi o wysokość — i to jest rzecz, której w bibliotece
// nie ma. Notujemy to tutaj, bo próba jest właściwym miejscem na taką wiedzę.
TRIAL("Frame: pusty węzeł nie zajmuje miejsca, a tło i tak jest") {
    ui::Root top;

    top.skin(painted());

    const auto frame = std::make_shared<ui::Frame>();
    const auto column = std::make_shared<ui::Column>();

    const auto apart = std::make_shared<ui::Node>();

    column->children().add(std::make_shared<ui::Span>("ab"));
    column->children().add(apart);
    column->children().add(std::make_shared<ui::Span>("cd"));

    frame->children().add(column);
    top.children().add(frame);

    const View made = top.render({{0, 0}, {10, 6}});

    same(read(made, 1), std::string{"│ab      │"});
    same(read(made, 2), std::string{"│cd      │"});

    // Prześwit obok napisu i wiersz pod ostatnim dzieckiem — w tle okna.
    same(made.at({5, 1}).style.background, (Color{false, 0, 0, 160}));
    same(made.at({3, 3}).style.background, (Color{false, 0, 0, 160}));
}


// --- Znaki dwukolumnowe ------------------------------------------------------------------------
//
// Skórka może rysować ramkę znakami CJK, a te zajmują dwie kratki. Bok jest wtedy dwiema kratkami,
// krawędź pozioma idzie co dwie, a złączki stają na szerokości boku. Dla zwykłych znaków wszystko
// wychodzi na jeden — i dlatego żaden wcześniejszy odcisk nie ma prawa się zmienić.

namespace {

ui::Skin widened() {
    return ui::Skin{{},
                    {{"talcite.line.thin.upper.left", U'口'},
                     {"talcite.line.thin.upper.right", U'口'},
                     {"talcite.line.thin.lower.left", U'口'},
                     {"talcite.line.thin.lower.right", U'口'},
                     {"talcite.line.thin.across", U'一'},
                     {"talcite.line.thin.upright", U'丨'},
                     {"talcite.line.thin.left.joint", U'卜'},
                     {"talcite.line.thin.right.joint", U'卜'}}};
}

}

TRIAL("Frame: znaki dwukolumnowe — bok zabiera dwie kratki, wnętrze zaczyna się dalej") {
    ui::Root top;

    top.skin(widened());

    const auto frame = std::make_shared<ui::Frame>();
    const auto inner = std::make_shared<Box>(ui::Size{20, 20}, U'x');

    frame->children().add(inner);
    top.children().add(frame);

    (void)top.render({{0, 0}, {12, 5}});

    same(inner->area().at.x, 2);
    same(inner->area().at.y, 1);
    same(inner->area().size.wide, 8);
    same(inner->area().size.high, 3);

    same(frame->wants({80, 25}).wide, 24);
}

TRIAL("Frame: znaki dwukolumnowe — krawędź idzie co dwie kratki, reszta zostaje tłem") {
    ui::Root top;

    top.skin(widened());

    const auto frame = std::make_shared<ui::Frame>();

    frame->children().add(std::make_shared<Box>(ui::Size{20, 20}, U'x'));
    top.children().add(frame);

    // Wnętrze ma pięć kratek: dwa znaki poziome zajmują cztery, piąta zostaje spacją — nie połówką.
    const View made = top.render({{0, 0}, {9, 3}});

    same(read(made, 0), std::string{"口一一 口"});
    same(read(made, 1), std::string{"丨xxxxx丨"});
    same(read(made, 2), std::string{"口一一 口"});
}

TRIAL("Frame: znaki dwukolumnowe — złączka staje na szerokości boku") {
    ui::Root top;

    top.skin(widened());

    const auto frame = std::make_shared<ui::Frame>();
    const auto column = std::make_shared<ui::Column>();

    column->children().add(std::make_shared<Box>(ui::Size{6, 1}, U'x'));
    column->children().add(std::make_shared<ui::Line>());
    column->children().add(std::make_shared<Box>(ui::Size{6, 1}, U'y'));

    frame->children().add(column);
    top.children().add(frame);

    const View made = top.render({{0, 0}, {10, 5}});

    same(read(made, 1), std::string{"丨xxxxxx丨"});
    same(read(made, 2), std::string{"卜一一一卜"});
    same(read(made, 3), std::string{"丨yyyyyy丨"});

    // Nieparzyste wnętrze: ostatni znak kreski się nie mieści, a złączka i tak wchodzi — bo kreska
    // doszła tak daleko, jak mogła.
    const View odd = top.render({{0, 0}, {9, 5}});

    same(read(odd, 2), std::string{"卜一一 卜"});
}
