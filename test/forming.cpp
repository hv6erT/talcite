#include "proving.hpp"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>

#include "talcite/box.hpp"
#include "talcite/button.hpp"
#include "talcite/column.hpp"
#include "talcite/encoding.hpp"
#include "talcite/field.hpp"
#include "talcite/form.hpp"
#include "talcite/root.hpp"
#include "talcite/span.hpp"
#include "talcite/tick.hpp"
#include "talcite/modals.hpp"

using namespace talcite;

namespace {

std::string read(const View& one, int y) {
    std::string made;

    for (int x = 0; x < one.size().wide; ++x) {
        const char32_t letter = one.at({x, y}).letter;

        if (letter != 0) talcite::utf8::encode(letter, made);
    }

    while (!made.empty() && made.back() == ' ') made.pop_back();

    return made;
}

bool shown(const View& one, const std::string& what) {
    for (int y = 0; y < one.size().high; ++y)
        if (read(one, y).find(what) != std::string::npos) return true;

    return false;
}

// Zasłona stawia pudełko pośrodku ekranu, więc próby liczą wiersze od górnej krawędzi ramki
// i porównują bez odstępu z lewej — kształt, nie miejsce.
int framed(const View& one) {
    for (int y = 0; y < one.size().high; ++y) {
        const std::string row = read(one, y);

        if (row.find("╔") != std::string::npos || row.find("┌") != std::string::npos) return y;
    }

    return -1;
}

std::string lined(const View& one, int y) {
    std::string made = read(one, y);

    made.erase(0, made.find_first_not_of(' '));

    return made;
}

void pressed(ui::Root& top, ui::Code code, ui::Mods mods = {}) {
    ui::KeyEvent one{ui::Key{code, mods}};

    (void)top.dispatch(one);
}

// Okno z otwartym formularzem: treść i przyciski **przed otwarciem**, bo ognisko stawia się
// w chwili otwarcia.
struct Opened {
    ui::Root top;
    std::shared_ptr<ui::Modals> window = std::make_shared<ui::Modals>();
    std::shared_ptr<ui::Form> one;

    explicit Opened(std::string title, ui::FormShape style = {}) {
        top.children().add(window);

        one = std::make_shared<ui::Form>(std::move(title), style);
    }

    void open() { window->open(one); }

    View drawn(ui::Size size) { return top.render({{0, 0}, size}); }
};

}

// --- Kształt ----------------------------------------------------------------------------------

// Tytuł w ramce, treść pod nim, pusty wiersz, przyciski pośrodku — jak okno porządku w Midnight
// Commanderze. Formularz nie maluje nic własnego; wszystko, co widać, to ramka, kolumna, rząd.
TRIAL("Form: tytuł w ramce, treść, odstęp i przyciski pośrodku") {
    Opened made{"Sort"};

    made.one->body()->children().add(std::make_shared<ui::Span>("Name"));
    made.one->body()->children().add(std::make_shared<ui::Span>("Size"));
    (void)made.one->buttons().add("OK");
    (void)made.one->buttons().add("Cancel");
    made.open();

    const View seen = made.drawn({30, 10});
    const int at = framed(seen);

    ought(at >= 0);
    same(lined(seen, at), std::string{"╔═════ Sort ══════╗"});
    same(lined(seen, at + 1), std::string{"║Name             ║"});
    same(lined(seen, at + 2), std::string{"║Size             ║"});
    same(lined(seen, at + 3), std::string{"║                 ║"});
    same(lined(seen, at + 4), std::string{"║[ OK ] [ Cancel ]║"});
    same(lined(seen, at + 5), std::string{"╚═════════════════╝"});
}

TRIAL("Form: przyciski stoją pośrodku, gdy treść jest szersza") {
    Opened made{"Filter"};

    made.one->body()->children().add(std::make_shared<ui::Span>("A long line of text here"));
    (void)made.one->buttons().add("OK");
    made.open();

    const View seen = made.drawn({40, 10});
    const int at = framed(seen);

    same(lined(seen, at + 1), std::string{"║A long line of text here║"});
    same(lined(seen, at + 3), std::string{"║         [ OK ]         ║"});
}

// Przycisków więcej, niż zmieści szerokość: rząd łamie się na dwie linie i żaden nie znika.
TRIAL("Form: przyciski, które się nie mieszczą, schodzą do następnej linii") {
    Opened made{"Many"};

    made.one->body()->children().add(std::make_shared<ui::Span>("Text"));

    for (const char* each : {"First", "Second", "Third", "Fourth", "Fifth"}) (void)made.one->buttons().add(each);

    made.open();

    const View seen = made.drawn({30, 12});
    const int at = framed(seen);

    same(lined(seen, at + 3), std::string{"║[ First ] [ Second ]║"});
    same(lined(seen, at + 4), std::string{"║[ Third ] [ Fourth ]║"});
    same(lined(seen, at + 5), std::string{"║     [ Fifth ]      ║"});
    same(lined(seen, at + 6), std::string{"╚════════════════════╝"});
}

// **Treść, która wypełnia wysokość**, dostaje ten sam pokój przy pytaniu o rozmiar i przy
// malowaniu: formularz odkłada najpierw odstęp i przyciski. Pudełko z życzeniem „ile chcę, ale
// zostaw wiersz na to, co pode mną" stoi wtedy tuż nad wierszem stanu, a ten tuż nad odstępem —
// bez pustych wierszy. Dawniej treść pytana była pierwsza, z pokojem razem z przyciskami, a przy
// malowaniu dostawała tylko to, co wcześniej chciała, i pudełko kurczyło się drugi raz.
TRIAL("Form: treść wypełniająca wysokość stoi tuż nad przyciskami, bez pustych wierszy") {
    Opened made{"Card"};

    const auto lines = std::make_shared<ui::Column>();

    for (int at = 0; at < 20; ++at) lines->children().add(std::make_shared<ui::Span>("line " + std::to_string(at)));

    const std::weak_ptr<ui::Column> inside = lines;

    const auto bounded = std::make_shared<ui::Box>([inside](ui::Size room) {
        const int most = std::max(room.high - 1, 0);
        const auto kept = inside.lock();

        return ui::Size{room.wide, kept ? std::min(kept->wants({room.wide, most}).high, most) : most};
    });

    bounded->children().add(lines);

    made.one->body()->children().add(bounded);
    made.one->body()->children().add(std::make_shared<ui::Span>("status"));
    (void)made.one->buttons().add("OK");
    made.open();

    const View seen = made.drawn({30, 12});
    const int at = framed(seen);

    // Pokój ma dwanaście wierszy, cień bierze jeden: ramka, sześć wierszy treści, stan, odstęp,
    // przyciski, ramka.
    same(at, 0);
    ought(lined(seen, at + 1).starts_with("║line 0"));
    ought(lined(seen, at + 6).starts_with("║line 5"));
    ought(lined(seen, at + 7).starts_with("║status"));
    ought(lined(seen, at + 8).find_first_not_of("║ ") == std::string::npos);
    ought(lined(seen, at + 9).find("[ OK ]") != std::string::npos);
    ought(lined(seen, at + 10).starts_with("╚"));
}

// Bez przycisków nie ma ani ich wiersza, ani odstępu przed nimi — `Gap` pyta obu sąsiadów.
TRIAL("Form: bez przycisków nie ma odstępu ani wiersza przycisków") {
    Opened made{"Note"};

    made.one->body()->children().add(std::make_shared<ui::Span>("Just this"));
    made.open();

    const View seen = made.drawn({30, 10});
    const int at = framed(seen);

    same(lined(seen, at), std::string{"╔═ Note ══╗"});
    same(lined(seen, at + 1), std::string{"║Just this║"});
    same(lined(seen, at + 2), std::string{"╚═════════╝"});
}

TRIAL("Form: bez treści nie ma odstępu, same przyciski") {
    Opened made{"Ask"};

    (void)made.one->buttons().add("Yes");
    (void)made.one->buttons().add("No");
    made.open();

    const View seen = made.drawn({30, 10});
    const int at = framed(seen);

    same(lined(seen, at + 1), std::string{"║[ Yes ] [ No ]║"});
    same(lined(seen, at + 2), std::string{"╚══════════════╝"});
}

TRIAL("Form: tytuł, styl ramki i cień idą z wołającego") {
    Opened made{"Thin", ui::FormShape{ui::Border::thin, false}};

    (void)made.one->buttons().add("OK");
    made.open();

    same(made.one->title(), std::string{"Thin"});

    made.one->title("Other");

    same(made.one->title(), std::string{"Other"});

    const View seen = made.drawn({30, 10});

    ought(shown(seen, "┌"));
    ought(shown(seen, "Other"));
    ought(!shown(seen, "Thin"));
}

// Ramka liczy szerokość z treści i tytuł rysuje w tym, co zostało — okno z samym `[ OK ]`
// i tytułem `Make directory` obcinałoby tytuł. Formularz chce nie mniej, niż tytuł potrzebuje.
TRIAL("Form: okno nie jest węższe niż jego tytuł") {
    Opened made{"Make directory"};

    (void)made.one->buttons().add("OK");
    made.open();

    const View seen = made.drawn({40, 10});
    const int at = framed(seen);

    same(lined(seen, at), std::string{"╔═ Make directory ═╗"});
    same(lined(seen, at + 1), std::string{"║      [ OK ]      ║"});
}

// --- Przyciski --------------------------------------------------------------------------------

// `add` oddaje pełny przycisk: własne `on<PickEvent>`, `primary`, `keys`. Formularz nie wiesza
// na nim niczego i sam się nie zamyka — zamknięcie jest częścią czynności wołającego.
TRIAL("Form: przycisk robi to, co dopiął wołający, i okno samo się nie zamyka") {
    Opened made{"Ask"};

    int said = 0;

    const std::shared_ptr<ui::Button> ok = made.one->buttons().add("OK");

    ok->primary(true);
    ok->keys().add(ui::Key{U'O'});

    (void)ok->on<ui::PickEvent>([&said](ui::PickEvent&) {
        ++said;

        return true;
    });

    made.open();

    (void)made.drawn({30, 10});

    same(made.one->buttons().many(), std::size_t{1});

    ui::KeyEvent letter{ui::Key{U'O'}};

    ought(made.top.dispatch(letter));
    same(said, 1);
    ought(!made.window->empty());

    ought(shown(made.drawn({30, 10}), "[< OK >]"));
}

// Formularz nie ma własnych ról, więc jego przycisk zostaje zwykłym przyciskiem — rolę rozdaje
// dopiero komunikat, który ma rodzinę ról.
TRIAL("Form: przycisk zostaje przy roli zwykłego przycisku") {
    Opened made{"Ask"};

    same(made.one->buttons().add("OK")->role(), std::string{"talcite.button"});
}

// Przyciski sięga się numerem, jak dzieci węzła; numer spoza rzędu to błąd wołającego.
TRIAL("Form: przycisk sięga się numerem, a spoza rzędu rzuca") {
    Opened made{"Ask"};

    const std::shared_ptr<ui::Button> yes = made.one->buttons().add("Yes");
    const std::shared_ptr<ui::Button> no = made.one->buttons().add("No");

    same(made.one->buttons()[0], yes);
    same(made.one->buttons()[1], no);

    bool threw = false;

    try {
        (void)made.one->buttons()[2];
    } catch (const std::out_of_range&) {
        threw = true;
    }

    ought(threw);
}

TRIAL("Form: clear zabiera przyciski razem z wierszem") {
    Opened made{"Ask"};

    (void)made.one->buttons().add("Yes");
    (void)made.one->buttons().add("No");
    made.open();

    ought(shown(made.drawn({30, 10}), "[ Yes ]"));

    made.one->buttons().clear();

    ought(made.one->buttons().empty());
    ought(!shown(made.drawn({30, 10}), "[ Yes ]"));
}

// --- Ognisko ----------------------------------------------------------------------------------

// Ognisko wchodzi na pierwsze, co je bierze — pole w treści — a Tab chodzi po treści i po
// przyciskach jednym kołem, bo to jedna zasłona.
TRIAL("Form: Tab chodzi po treści i przyciskach jednym kołem") {
    Opened made{"Name"};

    const auto field = std::make_shared<ui::Field>(10);
    const auto tick = std::make_shared<ui::Tick>("Only", false);

    made.one->body()->children().add(field);
    made.one->body()->children().add(tick);

    const std::shared_ptr<ui::Button> ok = made.one->buttons().add("OK");
    const std::shared_ptr<ui::Button> cancel = made.one->buttons().add("Cancel");

    made.open();

    (void)made.drawn({30, 10});

    ought(field->focused());

    pressed(made.top, ui::Code::tab);
    ought(tick->focused());

    pressed(made.top, ui::Code::tab);
    ought(ok->focused());

    pressed(made.top, ui::Code::tab);
    ought(cancel->focused());

    pressed(made.top, ui::Code::tab);
    ought(field->focused());
}

// Escape nie zamyka sam z siebie — jak w komunikacie i w każdej zasłonie.
TRIAL("Form: Esc zamyka dopiero z uchwytem") {
    Opened made{"Ask"};

    (void)made.one->buttons().add("OK");
    made.open();

    (void)made.drawn({30, 10});

    pressed(made.top, ui::Code::escape);

    ought(!made.window->empty());

    const std::weak_ptr<ui::Form> weak = made.one;

    (void)made.one->on<ui::KeyEvent>([weak](ui::KeyEvent& what) {
        if (what.key().code() != ui::Code::escape) return false;

        if (const auto mine = weak.lock()) mine->close();

        return true;
    });

    pressed(made.top, ui::Code::escape);

    ought(made.window->empty());
}
