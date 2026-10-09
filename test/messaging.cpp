#include "proving.hpp"

#include <memory>
#include <string>
#include <vector>

#include "talcite/button.hpp"
#include "talcite/modals.hpp"
#include "talcite/encoding.hpp"
#include "talcite/message.hpp"
#include "talcite/root.hpp"
#include "talcite/skin.hpp"
#include "talcite/span.hpp"

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

// Czy napis widać gdziekolwiek w klatce. Przy próbach, w których pudełko pojawia się i znika,
// wiersz przestaje być stały — a sprawdzamy wtedy obecność, nie miejsce.
bool shown(const View& one, const std::string& what) {
    for (int y = 0; y < one.size().high; ++y)
        if (read(one, y).find(what) != std::string::npos) return true;

    return false;
}

void pressed(ui::Root& top, ui::Code code, ui::Mods mods = {}) {
    ui::KeyEvent one{ui::Key{code, mods}};

    (void)top.dispatch(one);
}

void typed(ui::Root& top, char32_t letter) {
    ui::KeyEvent one{ui::Key{letter}};

    (void)top.dispatch(one);
}

// Jedna odpowiedź w próbie: napis i jej klawisz aktywny. W bibliotece takiej struktury nie ma —
// odpowiedź **jest** przyciskiem — ale próby budują ich dziesiątki i jedna linijka na sztukę
// czyta się lepiej niż trzy.
struct Said {
    std::string text;
    char32_t key = 0;
};

// Okno z otwartym komunikatem — tak, jak używa się go naprawdę: **przyciski przed otwarciem**, bo
// ognisko stawia się w chwili otwarcia i tego, co przyjdzie potem, samo nie dogoni.
struct Opened {
    ui::Root top;
    std::shared_ptr<ui::Modals> window = std::make_shared<ui::Modals>();
    std::shared_ptr<ui::Message> one;

    // Która odpowiedź padła i ile razy okno się zamknęło — jedno i drugie tak, jak dowiaduje się
    // o tym aplikacja: **czynność siedzi na przycisku**, a zamknięcie ogłasza zasłona.
    int said = -99;
    int closed = 0;

    Opened(std::string title, std::string text, std::vector<Said> answers = {},
          ui::MessageShape style = {}) {
        top.children().add(window);

        one = std::make_shared<ui::Message>(std::move(title), std::move(text), style);

        for (std::size_t at = 0; at < answers.size(); ++at) {
            const std::shared_ptr<ui::Button> made =
                one->buttons().add(std::move(answers[at].text));

            if (answers[at].key != 0) made->keys().add(answers[at].key);

            const int which = static_cast<int>(at);

            // **Zamknięcie jest częścią czynności odpowiedzi**, więc woła je wołający — okno
            // samo z siebie nie schodzi z ekranu.
            (void)made->on<ui::PickEvent>([this, which](ui::PickEvent&) {
                said = which;

                one->close();

                return true;
            });
        }

        (void)one->on<ui::CloseEvent>([this](ui::CloseEvent&) {
            ++closed;

            return true;
        });

        window->open(one);
    }

    View drawn(ui::Size size) { return top.render({{0, 0}, size}); }
};

}

// --- Kształt ----------------------------------------------------------------------------------

TRIAL("Message: tytuł nad pudełkiem treści, przyciski w drugim pudełku") {
    Opened made{"Error", "Nie da się.", {{"OK", U'O'}}};

    const View seen = made.drawn({30, 12});

    same(read(seen, 0), std::string{"      ╔═════════════╗"});
    same(read(seen, 1), std::string{"      ║    Error    ║"});
    same(read(seen, 2), std::string{"      ║┌───────────┐║"});
    same(read(seen, 3), std::string{"      ║│Nie da się.│║"});
    same(read(seen, 4), std::string{"      ║└───────────┘║"});
    same(read(seen, 5), std::string{"      ║             ║"});
    same(read(seen, 6), std::string{"      ║┌───────────┐║"});
    same(read(seen, 7), std::string{"      ║│  [ OK ]   │║"});
    same(read(seen, 8), std::string{"      ║└───────────┘║"});
    same(read(seen, 9), std::string{"      ╚═════════════╝"});
}

// Pojedyncza odpowiedź jest **zwykłym przyciskiem**, a nie podstawowym: tak wygląda `[ OK ]`
// w Midnight Commanderze. Enter i tak działa, bo naciska go ognisko, a nie oznaczenie.
TRIAL("Message: pojedyncza odpowiedź nie jest podstawowa") {
    Opened made{"Uwaga", "Coś.", {{"OK", U'O'}}};

    ought(!made.one->buttons()[0]->primary());
}

TRIAL("Message: bez tytułu nie ma wiersza tytułu") {
    Opened made{std::string{}, "Sama treść.", {{"OK", U'O'}}};

    const View seen = made.drawn({30, 12});

    same(read(seen, 2), std::string{"      ║┌───────────┐║"});
    same(read(seen, 3), std::string{"      ║│Sama treść.│║"});
}

TRIAL("Message: bez odpowiedzi nie ma drugiego pudełka") {
    Opened made{"Czekaj", "Przeglądam…"};

    const View seen = made.drawn({30, 10});

    same(read(seen, 1), std::string{"      ╔═════════════╗"});
    same(read(seen, 2), std::string{"      ║   Czekaj    ║"});
    same(read(seen, 3), std::string{"      ║┌───────────┐║"});
    same(read(seen, 4), std::string{"      ║│Przeglądam…│║"});
    same(read(seen, 5), std::string{"      ║└───────────┘║"});
    same(read(seen, 6), std::string{"      ╚═════════════╝"});
}

// Bez pudełek komunikat wygląda jak formularz: tytuł na krawędzi ramki, treść z kratką oddechu po
// obu stronach, przyciski pośrodku pod pustym wierszem.
TRIAL("Message: zgaszone pudełka dają tytuł na ramce i treść z oddechem") {
    Opened made{"Bez", "Goła treść.", {{"OK", U'O'}}, ui::MessageShape{ui::Border::twin, false}};

    const View seen = made.drawn({30, 10});

    same(read(seen, 2), std::string{"      ╔════ Bez ════╗"});
    same(read(seen, 3), std::string{"      ║ Goła treść. ║"});
    same(read(seen, 4), std::string{"      ║             ║"});
    same(read(seen, 5), std::string{"      ║   [ OK ]    ║"});
    same(read(seen, 6), std::string{"      ╚═════════════╝"});
    same(made.one->role(), std::string{"talcite.message"});
}

// Długa treść bez pudełek zawija się w pokoju węższym o oddech — żadne słowo nie ginie za ramką —
// a okno nie chce więcej niż pokój, także bardzo wąski.
TRIAL("Message: bez pudełek długa treść zawija się z oddechem, okno mieści się w pokoju") {
    Opened made{"T", "jeden dwa trzy cztery pięć sześć siedem osiem", {{"OK", U'O'}},
                ui::MessageShape{ui::Border::twin, false}};

    // Na każdej szerokości widać pierwsze i ostatnie słowo — zawijanie liczy się w tym samym pokoju,
    // w którym napis potem stoi.
    for (const int wide : {20, 21, 22, 23, 24, 25, 26}) {
        const View seen = made.drawn({wide, 14});

        bool first = false;
        bool last = false;

        for (int y = 0; y < 14; ++y) {
            first = first || read(seen, y).find("jeden") != std::string::npos;
            last = last || read(seen, y).find("osiem") != std::string::npos;
        }

        ought(first && last);
    }

    ought(made.one->wants({3, 10}).wide <= 3);
}

// Tytuł zmieniony po zbudowaniu idzie na ramkę, a długi tytuł poszerza okno, zamiast się uciąć.
TRIAL("Message: bez pudełek nowy tytuł stoi na ramce i poszerza okno") {
    Opened made{"A", "b", {{"OK", U'O'}}, ui::MessageShape{ui::Border::twin, false}};

    made.one->title("Bardzo długi tytuł");

    same(made.one->title(), std::string{"Bardzo długi tytuł"});

    const View seen = made.drawn({40, 10});

    ought(read(seen, 2).find("═ Bardzo długi tytuł ═") != std::string::npos);
}

// Bez treści nie ma wiersza treści ani odstępu; bez przycisków — odstępu ani rzędu.
TRIAL("Message: bez pudełek pusta treść i brak przycisków nie zostawiają dziur") {
    Opened bare{"Tytuł", "", {{"OK", U'O'}}, ui::MessageShape{ui::Border::twin, false}};

    const View one = bare.drawn({30, 10});

    int rows = 0;

    for (int y = 0; y < 10; ++y)
        if (!read(one, y).empty()) ++rows;

    same(rows, 3);

    Opened mute{"Tytuł", "Czekam.", {}, ui::MessageShape{ui::Border::twin, false}};

    const View two = mute.drawn({30, 10});

    rows = 0;

    for (int y = 0; y < 10; ++y)
        if (!read(two, y).empty()) ++rows;

    same(rows, 3);
}

TRIAL("Message: przyciski stoją pośrodku i mają odstęp") {
    Opened made{"Pytanie", "Plik już istnieje.", {{"Tak", U'T'}, {"Nie", U'N'}}};

    const View seen = made.drawn({40, 12});

    same(read(seen, 7), std::string{"        ║│ [ Tak ] [ Nie ]  │║"});
}

// --- Szerokość --------------------------------------------------------------------------------

TRIAL("Message: długa treść zawija się, zamiast rozpychać okno na cały ekran") {
    Opened made{"Uwaga",
               "To jest bardzo długie zdanie, które nie ma prawa rozepchnąć okna na całą "
               "szerokość ekranu, bo tak się okien nie robi.",
               {{"OK", U'O'}}};

    const View seen = made.drawn({60, 16});

    // Cztery kratki oddechu z boków i miejsce na cień — okno nie dotyka krawędzi.
    ought(read(seen, 1).size() > 40);
    same(seen.at({0, 1}).letter, U' ');
    same(seen.at({59, 1}).letter, U' ');

    // Treść poszła na kilka wierszy, a nie została ucięta.
    ought(read(seen, 4).find("To jest") != std::string::npos);
    ought(!read(seen, 5).empty());
}

TRIAL("Message: krótka treść nie rozdyma okna") {
    Opened made{"A", "b", {{"OK", U'O'}}};

    const View seen = made.drawn({60, 12});

    // Szerokość dyktuje najszersze dziecko — tu rząd przycisków, nie treść.
    same(read(seen, 3), std::string{"                        ║│  b   │║"});
    same(read(seen, 7), std::string{"                        ║│[ OK ]│║"});
}

// --- Odpowiedź --------------------------------------------------------------------------------

// **Czynność odpowiedzi mieszka na przycisku**, tak samo jak w pozycji menu i w polu legendy,
// a zamknięcie okna jest jej częścią — komunikat nie zamyka się sam.
TRIAL("Message: odpowiedź robi to, co dopiął wołający") {
    Opened made{"Pytanie", "Co robić?", {{"Tak", U'T'}, {"Nie", U'N'}, {"Przerwij", U'P'}}};

    (void)made.drawn({44, 14});

    made.one->buttons()[1]->pick();

    same(made.said, 1);
    ought(made.window->empty());
}

// **Escape sam z siebie nie zamyka**, bo zasłona przestała go brać — okno wykonujące czynność,
// której wołający nie może odwołać, jest decyzją za niego. Zamknięcie nadal ogłasza się osobnym
// zdarzeniem, bo przycisku „zamknij" nie ma i nie ma na czym wieszać czynności; zmienił się tylko
// ten, kto zamknięcie zaczyna.
TRIAL("Message: Esc zamyka dopiero z uchwytem, a zamknięcie się ogłasza") {
    Opened made{"Pytanie", "Co robić?", {{"Tak", U'T'}, {"Nie", U'N'}}};

    (void)made.drawn({44, 14});

    pressed(made.top, ui::Code::escape);

    same(made.closed, 0);
    ought(!made.window->empty());

    const std::weak_ptr<ui::Message> weak = made.one;

    (void)made.one->on<ui::KeyEvent>([weak](ui::KeyEvent& what) {
        if (what.key().code() != ui::Code::escape) return false;

        if (const auto mine = weak.lock()) mine->close();

        return true;
    });

    pressed(made.top, ui::Code::escape);

    same(made.said, -99);
    same(made.closed, 1);
    ought(made.window->empty());
}

// Zamknięcie ogłasza się **przy każdym** zamknięciu, nie tylko przy Escu — także wtedy, gdy woła
// je uchwyt wołającego. Kto słucha jednego zdarzenia, dowie się o obu drogach.
TRIAL("Message: zamknięcie z uchwytu też się ogłasza") {
    Opened made{"Pytanie", "Co robić?", {{"Tak", U'T'}, {"Nie", U'N'}}};

    (void)made.drawn({44, 14});

    made.one->buttons()[0]->pick();

    same(made.said, 0);
    same(made.closed, 1);
}

// Otwarcie stawia ognisko na pierwszej odpowiedzi, więc **Enter naciska tę, na której stoisz** —
// oznaczenie podstawowej rozstrzyga dopiero wtedy, gdy ognisko jest gdzie indziej. Tak jest
// w Midnight Commanderze i tak wychodzi z tego, że przycisk z ogniskiem sam bierze Enter.
TRIAL("Message: Enter naciska odpowiedź pod ogniskiem") {
    Opened made{"Pytanie", "Co robić?", {{"Tak", U'T'}, {"Nie", U'N'}}};

    made.one->buttons()[1]->primary(true);

    (void)made.drawn({44, 14});

    pressed(made.top, ui::Code::enter);

    same(made.said, 0);
}

// Oznaczenie podstawowej **niczego nie naciska** — rysuje klamrę ze strzałkami i tyle. Enter należy
// do tego, kto ma ognisko; gdy ognisko zeszło z przycisków, nie naciska nic.
TRIAL("Message: bez ogniska na przycisku Enter nic nie naciska") {
    Opened made{"Pytanie", "Co robić?", {{"Tak", U'T'}, {"Nie", U'N'}}};

    made.one->buttons()[1]->primary(true);

    (void)made.drawn({44, 14});

    // Ognisko wraca na samo okno — wtedy klawisz obsługuje dialog, a nie przycisk.
    made.one->focus();

    pressed(made.top, ui::Code::enter);

    same(made.said, -99);
    same(made.closed, 0);
    ought(!made.window->empty());
}

TRIAL("Message: klawisz aktywny działa z całego okna") {
    Opened made{"Pytanie", "Co robić?", {{"Tak", U'T'}, {"Nie", U'N'}}};

    (void)made.drawn({44, 14});

    typed(made.top, U'n');

    same(made.said, 1);
}

// Oznaczenie należy do **przycisku**, nie do komunikatu: `Message::base` zniknęło, bo mówiło
// przyciskom to, co przycisk i tak o sobie wie. Jedyności nikt nie pilnuje — dialog bierze
// pierwszy znaleziony w głąb i to jest reguła zamiast pilnowania.
TRIAL("Message: podstawową oznacza się na przycisku") {
    Opened made{"Pytanie", "Co robić?", {{"Tak", U'T'}, {"Nie", U'N'}}};

    ought(!made.one->buttons()[0]->primary());
    ought(!made.one->buttons()[1]->primary());

    made.one->buttons()[0]->primary(true);

    ought(made.one->buttons()[0]->primary());
    ought(!made.one->buttons()[1]->primary());

    made.one->buttons()[0]->primary(false);

    ought(!made.one->buttons()[0]->primary());
    ought(!made.one->buttons()[1]->primary());
}

TRIAL("Message: podstawowa rysuje się klamrą ze strzałkami") {
    Opened made{"Pytanie", "Co?", {{"Tak", U'T'}, {"Nie", U'N'}}};

    made.one->buttons()[0]->primary(true);

    same(read(made.drawn({40, 12}), 7), std::string{"        ║│[< Tak >] [ Nie ]│║"});
}

// --- Role -------------------------------------------------------------------------------------

TRIAL("Message: rdzeń roli rozdaje się na wszystkie części") {
    Opened made{"Error", "Nie da się.", {{"OK", U'O'}}};

    same(made.one->role(), std::string{"talcite.message"});

    ui::Styles these;

    Style window{};
    Style title{};
    Style text{};
    Style box{};
    Style button{};

    window.background = Color{false, 1, 1, 1};
    title.foreground = Color{false, 2, 2, 2};
    text.foreground = Color{false, 3, 3, 3};
    box.foreground = Color{false, 4, 4, 4};
    button.foreground = Color{false, 5, 5, 5};

    these["moj.blad"] = window;
    these["moj.blad.title"] = title;
    these["moj.blad.text"] = text;
    these["moj.blad.box"] = box;
    these["moj.blad.button"] = button;

    made.top.skin(ui::Skin{these});
    made.one->role("moj.blad");

    // Otwarcie stawia ognisko na przycisku, a ten pyta wtedy o końcówkę `.focus`. Tu chcemy
    // zobaczyć rolę spoczynkową, więc ognisko wraca na samo okno.
    made.one->focus();

    const View seen = made.drawn({30, 12});

    // Krawędź okna, tytuł, kreska pudełka, treść i przycisk — każde ze swojej roli.
    same(seen.at({6, 0}).style.background, (Color{false, 1, 1, 1}));
    same(seen.at({11, 1}).style.foreground, (Color{false, 2, 2, 2}));
    same(seen.at({7, 2}).style.foreground, (Color{false, 4, 4, 4}));
    same(seen.at({8, 3}).style.foreground, (Color{false, 3, 3, 3}));
    same(seen.at({10, 7}).style.foreground, (Color{false, 5, 5, 5}));
}

TRIAL("Message: przycisk pod ogniskiem bierze końcówkę swojego rdzenia") {
    Opened made{"Error", "Nie da się.", {{"OK", U'O'}}};

    ui::Styles these;

    Style hot{};

    hot.foreground = Color{false, 9, 9, 9};

    these["moj.blad.button.focus"] = hot;

    made.top.skin(ui::Skin{these});
    made.one->role("moj.blad");

    made.one->buttons()[0]->focus();

    same(made.drawn({30, 12}).at({10, 7}).style.foreground, (Color{false, 9, 9, 9}));
}

// --- Budowa -----------------------------------------------------------------------------------

// Komunikat składa się sam i nie ma drugiej drogi, którą dałoby się go złożyć źle — dokładnie tak,
// jak pasek menu nie przyjmuje treści inaczej niż przez `content()`.
TRIAL("Message: nie przyjmuje dziecka z zewnątrz") {
    ui::Message one{"Tytuł", "Treść"};

    bool threw = false;

    try {
        one.children().add(std::make_shared<ui::Span>("obce"));
    } catch (const std::logic_error&) {
        threw = true;
    }

    ought(threw);
}

TRIAL("Message: treść i tytuł da się zmienić po zbudowaniu") {
    Opened made{"Stary", "Stara treść.", {{"OK", U'O'}}};

    made.one->title("Nowy");
    made.one->text("Nowa treść.");

    same(made.one->title(), std::string{"Nowy"});
    same(made.one->text(), std::string{"Nowa treść."});

    const View seen = made.drawn({30, 12});

    ought(read(seen, 1).find("Nowy") != std::string::npos);
    ought(read(seen, 3).find("Nowa treść.") != std::string::npos);
    ought(read(seen, 3).find("Stara") == std::string::npos);
}

// **To jest próba na błąd, dla którego cała przebudowa się zaczęła.** Treść dopinana warunkowo
// przepadała dla każdego, kto zbudował komunikat bez niej i nadał ją potem: napis szedł na węzeł,
// który nie był niczyim dzieckiem, i nie pokazywał się nigdy. Teraz pudełko treści stoi w kolumnie
// od początku, a to, czy je widać, wychodzi z rozmiaru.
TRIAL("Message: treść nadana po zbudowaniu pokazuje się") {
    Opened made{"Uwaga", std::string{}, {{"OK", U'O'}}};

    ought(!shown(made.drawn({30, 12}), "Jest treść."));

    made.one->text("Jest treść.");

    ought(shown(made.drawn({30, 12}), "Jest treść."));
}

// To samo od drugiej strony: pudełko treści znika, gdy treść zabrać, i nie zostawia po sobie ani
// obwódki, ani wiersza odstępu.
TRIAL("Message: treść zabrana po zbudowaniu chowa pudełko") {
    Opened made{"Uwaga", "Była treść.", {{"OK", U'O'}}};

    const View was = made.drawn({30, 12});

    made.one->text(std::string{});

    const View seen = made.drawn({30, 12});

    ought(!shown(seen, "Była treść."));
    ought(shown(seen, "[ OK ]"));

    // Bez treści okno jest niższe o pudełko i o wiersz między pudełkami.
    ought(read(seen, 9).empty());
    ought(!read(was, 9).empty());
}

TRIAL("Message: odpowiedź dołożona po zbudowaniu działa jak każda inna") {
    Opened made{"Pytanie", "Co robić?", {{"Tak", U'T'}}};

    int seen = 0;

    const std::shared_ptr<ui::Button> more = made.one->buttons().add("Nie");

    more->keys().add(U'n');

    (void)more->on<ui::PickEvent>([&seen](ui::PickEvent&) {
        ++seen;

        return true;
    });

    ought(shown(made.drawn({44, 14}), "[ Nie ]"));

    typed(made.top, U'n');

    same(seen, 1);
}

TRIAL("Message: pierwsza odpowiedź dołożona po zbudowaniu pokazuje drugie pudełko") {
    Opened made{"Czekaj", "Przeglądam…"};

    ought(!shown(made.drawn({30, 12}), "[ Stop ]"));

    made.one->buttons().add("Stop");

    ought(shown(made.drawn({30, 12}), "[ Stop ]"));
}

TRIAL("Message: clear() zabiera odpowiedzi razem z pudełkiem") {
    Opened made{"Pytanie", "Co robić?", {{"Tak", U'T'}, {"Nie", U'N'}}};

    (void)made.drawn({44, 14});

    made.one->buttons().clear();

    same(made.one->buttons().many(), std::size_t{0});

    const View seen = made.drawn({44, 14});

    ought(!shown(seen, "[ Tak ]"));
    ought(shown(seen, "Co robić?"));
}

// **Odpowiedź bez czynności nie robi nic** — i tak ma być. Biblioteka nie dokłada przyciskowi
// zachowania, którego wołający na nim nie powiesił, więc świeżo dołożona odpowiedź nie zamyka
// okna i niczego nie ogłasza.
TRIAL("Message: odpowiedź bez uchwytu niczego nie robi") {
    Opened made{"Pytanie", "Co robić?", {{"Tak", U'T'}, {"Nie", U'N'}}};

    made.one->buttons().clear();

    const std::shared_ptr<ui::Button> only = made.one->buttons().add("Przerwij");

    (void)made.drawn({44, 14});

    only->pick();

    same(made.said, -99);
    same(made.closed, 0);
    ought(!made.window->empty());
}

// Przycisk zdjęty z okna **dalej ma swoją czynność** — należy ona do wołającego i nikt mu jej nie
// zabiera ani nie odbiera jej mocy. Skoro ta czynność zamyka okno, to okno się zamknie, choć
// przycisk nie stoi już w drzewie: biblioteka niczego tu za nikogo nie rozstrzyga.
TRIAL("Message: przycisk zdjęty przez clear() dalej robi swoje") {
    Opened made{"Pytanie", "Co robić?", {{"Tak", U'T'}, {"Nie", U'N'}}};

    const std::shared_ptr<ui::Button> was = made.one->buttons()[0];

    made.one->buttons().clear();

    (void)made.drawn({44, 14});

    was->pick();

    same(made.said, 0);
    same(made.closed, 1);
}

// Rola odpowiedzi rozdaje się i **przy dołożeniu**, i przy zmianie rdzenia — w obie strony, bez
// względu na to, co było pierwsze.
TRIAL("Message: odpowiedź dołożona po zmianie rdzenia bierze jego końcówkę") {
    Opened made{"Error", "Nie da się.", {{"OK", U'O'}}};

    made.one->role("moj.blad");

    const std::shared_ptr<ui::Button> more = made.one->buttons().add("Anuluj");

    same(more->role(), std::string{"moj.blad.button"});

    made.one->role("inny.blad");

    same(more->role(), std::string{"inny.blad.button"});
    same(made.one->buttons()[0]->role(), std::string{"inny.blad.button"});
}
