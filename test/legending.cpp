#include "proving.hpp"

#include <memory>
#include <string>

#include "talcite/encoding.hpp"
#include "talcite/legenditem.hpp"
#include "talcite/legend.hpp"
#include "talcite/menu.hpp"
#include "talcite/menuitem.hpp"
#include "talcite/menubar.hpp"
#include "talcite/root.hpp"
#include "talcite/skin.hpp"

using namespace talcite;

// Legenda jest oknem zewnętrznym, a wszystko inne siedzi w jej treści — więc prawie każda próba
// zaczyna się tak samo: korzeń, okno, coś w środku, co bierze ognisko.
#include "doing.hpp"

namespace {

class Box : public ui::Node {
public:
    Box(ui::Size wants, char32_t what) : wants_(wants), what_(what) {}

    [[nodiscard]] bool focusable() const override { return true; }

    [[nodiscard]] ui::Size wants(ui::Size) const override { return wants_; }

    int hit = 0;

protected:
    View paint(ui::Size given, ui::Area seen) override {
        View made{seen.size};

        for (int y = 0; y < given.high; ++y)
            for (int x = 0; x < given.wide; ++x)
                made.put({x - seen.at.x, y - seen.at.y}, what_, Style{});

        return made;
    }

    bool handle(ui::ClickEvent&) override {
        ++hit;

        return true;
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

TRIAL("Legenda: pola dzielą wiersz i stykają się bokami") {
    ui::Root top;

    const auto window = std::make_shared<ui::Legend>();

    (void)window->add(ui::Code::f1, "Kopiuj");
    (void)window->add(ui::Code::f2, "Wyjdź");
    top.children().add(window);

    const View made = top.render({{0, 0}, {16, 4}});

    same(read(made, 3), std::string{"1Kopiuj 2Wyjdź"});

    // Pola dzielą szerokość po równo i stoją jedno przy drugim.
    same(window->items()[0]->area(), (ui::Area{{0, 3}, {8, 1}}));
    same(window->items()[1]->area(), (ui::Area{{8, 3}, {8, 1}}));
}

// Reszta z dzielenia idzie do pól od lewej, więc pasek wypełnia wiersz co do kratki i nie zostaje
// ogryzek przy prawej krawędzi.
TRIAL("Legenda: reszta z dzielenia idzie do pól od lewej") {
    ui::Root top;

    const auto window = std::make_shared<ui::Legend>();

    for (int at = 0; at < 3; ++at) (void)window->add(ui::Code::f1, "");

    top.children().add(window);

    (void)top.render({{0, 0}, {11, 4}});

    same(window->items()[0]->area(), (ui::Area{{0, 3}, {4, 1}}));
    same(window->items()[1]->area(), (ui::Area{{4, 3}, {4, 1}}));
    same(window->items()[2]->area(), (ui::Area{{8, 3}, {3, 1}}));
}

// Etykietka wyprowadza się z klawisza, a nie stoi obok niego w osobnym polu — inaczej dałoby się
// napisać legendę, która kłamie o tym, co nacisnąć.
TRIAL("Legenda: pole nie bierze ogniska, także włączone") {
    ui::Legend window;

    const auto one = window.add(ui::Code::f5, "Kopiuj");

    ought(one->enabled());
    ought(!one->focusable());
}

TRIAL("Legenda: etykietka bierze się z klawisza") {
    same((ui::LegendItem{ui::Code::f1, ""}.tag()), std::string{"1"});
    same((ui::LegendItem{ui::Code::f5, ""}.tag()), std::string{"5"});
    same((ui::LegendItem{ui::Code::f10, ""}.tag()), std::string{"10"});
    same((ui::LegendItem{ui::Code::f12, ""}.tag()), std::string{"12"});
    same((ui::LegendItem{U'q', ""}.tag()), std::string{"q"});

    // Escape i reszta nazwanych klawiszy nie mają w Midnight Commanderze etykietki.
    same((ui::LegendItem{ui::Code::escape, ""}.tag()), std::string{});
    same((ui::LegendItem{ui::Key{}, ""}.tag()), std::string{});
}

TRIAL("Legenda: klawisz dochodzi bąbelkiem od ogniska") {
    ui::Root top;

    const auto window = std::make_shared<ui::Legend>();
    const auto body = std::make_shared<Box>(ui::Size{10, 3}, U'x');

    int done = 0;

    window->content(body);
    (void)proving::doing(window->add(ui::Code::f5, "Kopiuj"), [&done] { ++done; });
    top.children().add(window);

    body->focus();

    (void)top.render({{0, 0}, {16, 4}});

    ui::KeyEvent what{ui::Code::f5};

    ought(top.dispatch(what));
    same(done, 1);

    // Klawisz, którego legenda nie trzyma, nie jest jej sprawą.
    ui::KeyEvent other{ui::Code::f6};

    ought(!top.dispatch(other));
    same(done, 1);
}

// Ctrl+F5 i Alt+F5 to z założenia inne skróty niż F5, a legenda nie modeluje warstw pod
// modyfikatorami — więc ich nie bierze, zamiast udawać, że to to samo.
TRIAL("Legenda: ze sterującym nie łapie") {
    ui::Root top;

    const auto window = std::make_shared<ui::Legend>();
    const auto body = std::make_shared<Box>(ui::Size{10, 3}, U'x');

    int done = 0;

    window->content(body);
    (void)proving::doing(window->add(ui::Code::f5, "Kopiuj"), [&done] { ++done; });
    top.children().add(window);

    body->focus();

    (void)top.render({{0, 0}, {16, 4}});

    ui::KeyEvent held{ui::Key{ui::Code::f5, ui::Mods{true, false, false}}};

    ought(!top.dispatch(held));

    ui::KeyEvent aside{ui::Key{ui::Code::f5, ui::Mods{false, true, false}}};

    ought(!top.dispatch(aside));

    same(done, 0);
}

// Kliknięcie robi to samo, co klawisz, i **nie zabiera ogniska**: po kliknięciu „Kopiuj" kopiować
// ma się to, co było zaznaczone, a nie to, co akurat dostało ognisko.
TRIAL("Legenda: kliknięcie robi to samo i nie zabiera ogniska") {
    ui::Root top;

    const auto window = std::make_shared<ui::Legend>();
    const auto body = std::make_shared<Box>(ui::Size{10, 3}, U'x');

    int done = 0;

    window->content(body);
    (void)proving::doing(window->add(ui::Code::f5, "Kopiuj"), [&done] { ++done; });
    top.children().add(window);

    body->focus();

    (void)top.render({{0, 0}, {16, 4}});

    ui::ClickEvent what{{3, 3}, ui::Mouse::left};

    ought(top.dispatch(what));
    same(done, 1);
    same(top.focused_node(), static_cast<ui::Node*>(body.get()));
}

TRIAL("Legenda: pole niedostępne milczy na klawisz i na klik") {
    ui::Root top;

    const auto window = std::make_shared<ui::Legend>();
    const auto body = std::make_shared<Box>(ui::Size{10, 3}, U'x');
    const auto one = window->add(ui::Code::f5, "Kopiuj");

    int done = 0;

    (void)one->on<ui::PickEvent>([&done](ui::PickEvent&) {
        ++done;
        return true;
    });
    one->enabled(false);

    window->content(body);
    top.children().add(window);

    body->focus();

    (void)top.render({{0, 0}, {16, 4}});

    ui::KeyEvent what{ui::Code::f5};

    ought(!top.dispatch(what));

    ui::ClickEvent where{{3, 3}, ui::Mouse::left};

    // Klik pole bierze — bo w nie trafiono — ale czynu nie robi.
    ought(top.dispatch(where));

    same(done, 0);
}

TRIAL("Legenda: treść dostaje wszystkie wiersze poza ostatnim") {
    ui::Root top;

    const auto window = std::make_shared<ui::Legend>();
    const auto body = std::make_shared<Box>(ui::Size{ui::plenty, ui::plenty}, U'x');

    window->content(body);
    (void)window->add(ui::Code::f1, "Raz");
    top.children().add(window);

    const View made = top.render({{0, 0}, {6, 3}});

    same(read(made, 0), std::string{"xxxxxx"});
    same(read(made, 1), std::string{"xxxxxx"});
    same(read(made, 2), std::string{"1Raz"});
}

// Pusty pasek to pasek, którego nie ma, a nie pusta krecha na dole ekranu.
TRIAL("Legenda: bez pól nie zabiera wiersza") {
    ui::Root top;

    const auto window = std::make_shared<ui::Legend>();
    const auto body = std::make_shared<Box>(ui::Size{ui::plenty, ui::plenty}, U'x');

    window->content(body);
    top.children().add(window);

    const View made = top.render({{0, 0}, {6, 3}});

    same(read(made, 2), std::string{"xxxxxx"});
}

TRIAL("Legenda: za wąskie pole ucina nazwę, a potem etykietkę") {
    ui::Root top;

    const auto window = std::make_shared<ui::Legend>();

    (void)window->add(ui::Code::f1, "Kopiuj");
    (void)window->add(ui::Code::f2, "Wyjdź");
    top.children().add(window);

    const View made = top.render({{0, 0}, {8, 2}});

    // Cztery kratki na pole: etykietka i trzy na nazwę, z wielokropkiem.
    same(read(made, 1), std::string{"1Ko…2Wy…"});

    // Przy jednej kratce na pole zostaje sama etykietka i nic poza swój prostokąt nie wychodzi.
    const View tight = top.render({{0, 0}, {2, 2}});

    same(read(tight, 1), std::string{"12"});
}

// Litera bez różnicy wielkości — ta sama reguła, co w przycisku i w menu. Przy F-klawiszach nie ma
// czego zwijać, ale pole zgłoszone literą ma odpowiadać na obie jej postacie.
TRIAL("Legenda: litera odpowiada bez różnicy wielkości") {
    ui::Root top;

    const auto window = std::make_shared<ui::Legend>();
    const auto body = std::make_shared<Box>(ui::Size{10, 3}, U'x');

    int said = 0;

    const auto one = window->add(ui::Key{U'Q'}, "Wyjdź");

    (void)one->on<ui::PickEvent>([&said](ui::PickEvent&) {
        ++said;

        return true;
    });

    window->content(body);
    top.children().add(window);

    body->focus();

    ui::KeyEvent small{U'q'};

    ought(top.dispatch(small));
    same(said, 1);

    ui::KeyEvent big{U'Q'};

    ought(top.dispatch(big));
    same(said, 2);
}

TRIAL("Legenda: opróżnienie pozwala złożyć pasek od nowa") {
    ui::Root top;

    const auto window = std::make_shared<ui::Legend>();

    (void)window->add(ui::Code::f1, "Raz");
    (void)window->add(ui::Code::f2, "Dwa");
    top.children().add(window);

    same(static_cast<int>(window->items().size()), 2);
    same(window->capacity(), 3);

    window->clear();

    ought(window->items().empty());
    same(window->capacity(), 1);

    (void)window->add(ui::Code::f3, "Trzy");

    const View made = top.render({{0, 0}, {8, 2}});

    same(read(made, 1), std::string{"3Trzy"});
}

// Legenda jest oknem **zewnętrznym**, a pasek menu siedzi w jej treści. Dzięki temu rozwinięte menu
// dostaje wiersze do przedostatniego i nie ma jak zamalować paska klawiszy — jak w Midnight
// Commanderze.
TRIAL("Legenda: rozwinięte menu nie zamalowuje paska klawiszy") {
    ui::Root top;

    const auto legend = std::make_shared<ui::Legend>();
    const auto menus = std::make_shared<ui::Menubar>();

    menus->keys().add(ui::Key{ui::Code::f9});
    const auto one = menus->add("Plik", ui::MenuShape{ui::Border::thin, false});

    one->children().add(std::make_shared<ui::MenuItem>("Kopiuj"));
    one->children().add(std::make_shared<ui::MenuItem>("Wyjście"));
    one->children().add(std::make_shared<ui::MenuItem>("Jeszcze"));

    legend->content(menus);
    (void)legend->add(ui::Code::f5, "Kopiuj");
    top.children().add(legend);

    one->open();

    const View made = top.render({{0, 0}, {12, 7}});

    ought(one->opened());

    // Ostatni wiersz należy do legendy i nic go nie dotyka.
    same(read(made, 6), std::string{"5Kopiuj"});
}

// Otwarte menu jest modalne: klawisz, którego samo nie obsłuży, nie ma iść dalej w górę. Bez tego
// F5 przy rozwiniętym menu wywołałoby kopiowanie.
TRIAL("Legenda: przy otwartym menu klawisz legendy nie działa") {
    ui::Root top;

    const auto legend = std::make_shared<ui::Legend>();
    const auto menus = std::make_shared<ui::Menubar>();

    menus->keys().add(ui::Key{ui::Code::f9});
    const auto body = std::make_shared<Box>(ui::Size{10, 3}, U'x');
    const auto one = menus->add("Plik");

    int done = 0;

    one->children().add(std::make_shared<ui::MenuItem>("Kopiuj"));

    menus->content(body);
    legend->content(menus);
    (void)proving::doing(legend->add(ui::Code::f5, "Kopiuj"), [&done] { ++done; });
    top.children().add(legend);

    body->focus();

    (void)top.render({{0, 0}, {16, 7}});

    // Przy zamkniętym menu legenda działa.
    ui::KeyEvent first{ui::Code::f5};

    ought(top.dispatch(first));
    same(done, 1);

    // F9 bierze pasek, rozwinięcie wybiera się osobno — dopiero rozwinięte menu jest modalne.
    ui::KeyEvent open{ui::Code::f9};

    ought(top.dispatch(open));

    ui::KeyEvent drop{ui::Code::down};

    ought(top.dispatch(drop));
    ought(one->opened());

    ui::KeyEvent second{ui::Code::f5};

    ought(top.dispatch(second));
    same(done, 1);
}

// --- Grupa ------------------------------------------------------------------------------------

// Pole robi **pasek**, a nie wołający: wtedy rola, miejsce w drzewie i miejsce w capacity() biorą się
// z jednego wywołania i nie ma jak ich rozjechać.
TRIAL("Legenda: zdjęte pole znika też z drzewa") {
    ui::Root top;

    const auto window = std::make_shared<ui::Legend>();
    const auto one = window->add(ui::Code::f1, "Kopiuj");
    const auto two = window->add(ui::Code::f2, "Wyjdź");

    top.children().add(window);

    same(window->items().size(), std::size_t{2});
    ought(window->children().contains(two.get()));

    window->clear();

    ought(window->items().empty());
    ought(!window->children().contains(two.get()));
    ought(!window->children().contains(one.get()));

    // Opróżniony pasek składa się od nowa, a stare pola nie mają już z nim nic wspólnego.
    const auto again = window->add(ui::Code::f1, "Kopiuj");

    ought(window->children().contains(again.get()));

    same(read(top.render({{0, 0}, {16, 4}}), 3), std::string{"1Kopiuj"});
}

TRIAL("Legenda: rdzeń roli pola można podmienić") {
    ui::Root top;

    const auto window = std::make_shared<ui::Legend>();
    const auto one = window->add(ui::Code::f1, "Kopiuj");

    same(one->role(), std::string{"talcite.legend"});

    top.children().add(window);

    ui::Styles these;

    Style key{};
    Style text{};

    key.foreground = Color{false, 1, 1, 1};
    text.foreground = Color{false, 2, 2, 2};

    these["moj.pas.tag"] = key;
    these["moj.pas.text"] = text;

    top.skin(ui::Skin{these});
    one->role("moj.pas");

    const View made = top.render({{0, 0}, {16, 4}});

    same(made.at({0, 3}).style.foreground, (Color{false, 1, 1, 1}));
    same(made.at({1, 3}).style.foreground, (Color{false, 2, 2, 2}));
}

// Pole ma **zbiór** klawiszy, nie jeden klawisz: etykietkę rysuje pierwszy z nich, ale odpowiada
// każdy. To ta sama zdolność, co w pozycji menu, i bierze się z tego samego miejsca.
TRIAL("Legenda: pole odpowiada na każdy ze swoich klawiszy") {
    ui::Root top;

    const auto window = std::make_shared<ui::Legend>();
    const auto body = std::make_shared<Box>(ui::Size{10, 3}, U'x');

    int said = 0;

    const auto one = window->add(ui::Key{ui::Code::f10}, "Wyjdź");

    one->keys().add(ui::Key{U'q'});

    (void)one->on<ui::PickEvent>([&said](ui::PickEvent&) {
        ++said;

        return true;
    });

    window->content(body);
    top.children().add(window);

    body->focus();

    same(one->tag(), std::string{"10"});

    ui::KeyEvent named{ui::Code::f10};

    ought(top.dispatch(named));
    same(said, 1);

    ui::KeyEvent letter{U'Q'};

    ought(top.dispatch(letter));
    same(said, 2);
}

// Pasek rozdaje pola **zdarzeniem aktywnym**, więc uchwyt zawieszony z zewnątrz odpowiada na
// równych prawach z `handle` — tak samo jak w oknie dialogowym i w menu. Wcześniej pasek wołał
// `pick()` wprost i uchwyt na klawiszu aktywnym nie miał jak dojść.
TRIAL("Legenda: pole odpowiada na klawisz uchwytem zawieszonym z zewnątrz") {
    ui::Root top;

    const auto window = std::make_shared<ui::Legend>();
    const auto body = std::make_shared<Box>(ui::Size{10, 3}, U'x');

    int said = 0;

    const auto one = window->add(ui::Code::f5, "Kopiuj");

    (void)one->on<ui::ShortcutEvent>([&said](ui::ShortcutEvent&) {
        ++said;

        return true;
    });

    window->content(body);
    top.children().add(window);

    body->focus();

    ui::KeyEvent key{ui::Code::f5};

    ought(top.dispatch(key));
    same(said, 1);
}

// Rozdanie idzie **wyłącznie po polach paska**: treść nie ma prawa dostać klawisza aktywnego od
// legendy, bo pasek jest z boku układu, a nie nad treścią.
TRIAL("Legenda: rozdanie nie wchodzi w treść") {
    ui::Root top;

    const auto window = std::make_shared<ui::Legend>();
    const auto body = std::make_shared<Box>(ui::Size{10, 3}, U'x');

    int said = 0;

    (void)body->on<ui::ShortcutEvent>([&said](ui::ShortcutEvent&) {
        ++said;

        return true;
    });

    (void)window->add(ui::Code::f5, "Kopiuj");

    window->content(body);
    top.children().add(window);

    body->focus();

    ui::KeyEvent key{ui::Code::f7};

    ought(!top.dispatch(key));
    same(said, 0);
}

// **Litera aktywna zastępuje etykietkę**, a nie staje obok niej: pole z klawiszem-literą obecną
// w nazwie rysuje samą nazwę i podkreśla w niej tę literę, jak pozycja menu.
TRIAL("Legenda: klawisz-litera podkreśla się w nazwie zamiast etykietki") {
    ui::Root top;

    const auto window = std::make_shared<ui::Legend>();
    const auto one = window->add(ui::Key{U'k'}, "Kopiuj");

    top.children().add(window);

    const View made = top.render({{0, 0}, {6, 2}});

    same(read(made, 1), std::string{"Kopiuj"});
    ought(made.at({0, 1}).style.under);
    ought(!made.at({1, 1}).style.under);
}

// Litera, której w nazwie nie ma, nie ma czego podkreślać — więc wraca etykietka, bo inaczej
// byłby to skrót, o którym nikt się nie dowie.
TRIAL("Legenda: klawisz-litera spoza nazwy zostaje etykietką") {
    ui::Root top;

    const auto window = std::make_shared<ui::Legend>();
    const auto one = window->add(ui::Key{U'q'}, "Wyjdź");

    top.children().add(window);

    same(one->tag(), std::string{"q"});

    const View made = top.render({{0, 0}, {6, 2}});

    same(read(made, 1), std::string{"qWyjdź"});
}

// Klawisz funkcyjny nie ma litery, więc etykietka zostaje zawsze — tak jak w Midnight Commanderze.
TRIAL("Legenda: klawisz funkcyjny zostaje przy cyfrze") {
    ui::Root top;

    const auto window = std::make_shared<ui::Legend>();

    (void)window->add(ui::Code::f5, "Kopiuj");

    top.children().add(window);

    const View made = top.render({{0, 0}, {7, 2}});

    same(read(made, 1), std::string{"5Kopiuj"});
}

// Pole zgłoszone klawiszem funkcyjnym **zatrzymuje cyfrę**, nawet gdy ma w zapasie także literę:
// pasek czyta się jako ciąg numerów i dziura w tym ciągu byłaby gorsza niż brak podkreślenia.
// Litera i tak działa, i tak samo się podkreśla — tylko nie kosztuje cyfry.
TRIAL("Legenda: cyfra zostaje, choć pole ma też literę") {
    ui::Root top;

    const auto window = std::make_shared<ui::Legend>();
    const auto one = window->add(ui::Code::f5, "Kopiuj");

    one->keys().add(ui::Key{U'k'});

    top.children().add(window);

    const View made = top.render({{0, 0}, {7, 2}});

    same(read(made, 1), std::string{"5Kopiuj"});
    ought(made.at({1, 1}).style.under);
    ought(!made.at({2, 1}).style.under);
}

// **Modyfikatory są częścią klawisza**, więc pasek nie musi ich odsiewać: pole zgłoszone na F5 nie
// odpowiada na Ctrl+F5, a pole zgłoszone na Ctrl+F5 odpowiada wyłącznie na nie. Stał tu wcześniej
// ręczny odsiew, bo zbiór klawiszy nie umiał tego wyrazić.
TRIAL("Legenda: pole ze sterującym odpowiada tylko na swoją kombinację") {
    ui::Root top;

    const auto window = std::make_shared<ui::Legend>();
    const auto body = std::make_shared<Box>(ui::Size{10, 3}, U'x');

    int plain = 0;
    int held = 0;

    ui::Mods ctrl;
    ctrl.ctrl = true;

    const auto one = window->add(ui::Code::f5, "Kopiuj");
    const auto two = window->add(ui::Key{ui::Code::f5, ctrl}, "Inne");

    (void)one->on<ui::PickEvent>([&plain](ui::PickEvent&) { ++plain; return true; });
    (void)two->on<ui::PickEvent>([&held](ui::PickEvent&) { ++held; return true; });

    window->content(body);
    top.children().add(window);

    body->focus();

    ui::KeyEvent bare{ui::Key{ui::Code::f5}};

    ought(top.dispatch(bare));
    same(plain, 1);
    same(held, 0);

    ui::KeyEvent with{ui::Key{ui::Code::f5, ctrl}};

    ought(top.dispatch(with));
    same(plain, 1);
    same(held, 1);
}

// Treść jest jedna: nowa wypiera starą, a pusty wskaźnik opróżnia gniazdo. Stara treść wychodzi
// z drzewa całkiem, więc można ją włożyć gdzie indziej.
TRIAL("Legenda: nowa treść wypiera starą, a pusta opróżnia gniazdo") {
    ui::Legend window;

    const auto one = std::make_shared<Box>(ui::Size{4, 1}, U'a');
    const auto two = std::make_shared<Box>(ui::Size{4, 1}, U'b');

    window.content(one);
    window.content(two);

    same(window.content(), static_cast<ui::Node*>(two.get()));
    ought(one->parent() == nullptr);
    same(read(window.render({{0, 0}, {4, 2}}), 0), std::string{"bbbb"});

    window.content(nullptr);

    ought(window.content() == nullptr);
    ought(two->parent() == nullptr);
}
