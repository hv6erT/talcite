#include "talcite/list.hpp"

#include <algorithm>
#include <climits>

#include "talcite/skin.hpp"
#include "talcite/listitem.hpp"
#include "talcite/root.hpp"

#include "scrollbar.hpp"

namespace talcite::ui {

namespace {

// Trzy wiersze na ząbek kółka — ta sama stała i ten sam powód co w przewijaku.
constexpr int stepping = 3;

}

List::List(int wide, int high, ScrollShape shape)
    : shape_(shape), grip_(std::make_shared<Grip>()), wide_(wide), high_(high) {}

// Lista trzyma pozycje jako dzieci, tak samo jak menu — więc przyjmuje ich dowolnie wiele.
int List::capacity() const { return plenty; }

// Lista składa się z wierszy. `which()` i `current()` liczą numer wśród dzieci i sięga po nie jak po pozycje,
// więc obce dziecko rozjeżdżałoby numerację z tym, co widać.
bool List::accepts(const Node& one) const { return dynamic_cast<const ListItem*>(&one) != nullptr; }

// **Lista jest chętna zawsze, także pusta.** Pusty panel plików musi dać się wybrać Tabem, inaczej
// pusty katalog wyrzucałby ognisko w bok. Chętność jest tu przy okazji granicą wędrówki: chętny
// widżet jest dla Tabu liściem, więc Tab staje na liście, a nie na każdym jej wierszu.
//
// Wyłączenia całej listy **nie ma i nie ma go celowo**. Wyłączona byłaby niechętna, a wtedy Tab
// zszedłby w nią i stanął na wierszu — bo wiersz dalej byłby chętny. Wyłącza się pojedyncze
// pozycje; to samo pojęcie, co w menu.
bool List::focusable() const { return true; }

Size List::wants(Size room) const {
    return {std::min(wide_, room.wide), std::min(high_, room.high)};
}

ListItem* List::item(int which) const {
    if (which < 0 || which >= static_cast<int>(children().many())) return nullptr;

    return dynamic_cast<ListItem*>(children()[static_cast<std::size_t>(which)].get());
}

// Najbliższa dostępna pozycja w podanym kierunku, **bez zawijania**. Menu zawija, bo jest krótkie
// i widać je całe; lista na dziesięć tysięcy plików, która z ostatniego wiersza skacze na pierwszy,
// gubi człowieka. Tak samo rozstrzyga to panel Midnight Commandera.
int List::nearest(int from, int step) const {
    const int much = static_cast<int>(children().many());

    if (much == 0 || step == 0) return -1;

    for (int at = from + step; at >= 0 && at < much; at += step) {
        const ListItem* one = item(at);

        if (one != nullptr && one->enabled()) return at;
    }

    return -1;
}

// **Ognisko idzie na pozycję, a nie na listę** — i stąd bierze się cała reszta: klawisz zaczyna
// u wiersza, więc dopięty do niego uchwyt widzi go pierwszy, a numer bieżącego wiersza lista
// dostaje bąbelkowaniem, zamiast prowadzić własną księgowość obok ogniska.
//
// Gdy nie ma na czym stanąć — lista pusta albo same niedostępne pozycje — bierze ognisko sama.
void List::focus() {
    ListItem* one = item(which_);

    if (one == nullptr || !one->enabled()) one = item(nearest(-1, 1));

    if (one != nullptr) {
        one->focus();

        return;
    }

    Node::focus();
}

// **Bieżący wiersz ma dwie twarze pod dwiema nazwami**, tak samo w każdym widżecie z pozycjami:
// `which()` to jego numer, `current()` — on sam. Kto chce z wierszem coś zrobić — przeczytać
// napis, nacisnąć, porównać tożsamość — bierze `current()`; kto potrzebuje numeru do własnej
// tablicy obok, bierze `which()`. Pustka to `nullopt` i `nullptr`, bez umownej liczby. W środku
// numer siedzi jako `int` z `-1`, bo rachunek kroków w `nearest` idzie na nim.
std::optional<std::size_t> List::which() const {
    if (item(which_) == nullptr) return std::nullopt;

    return static_cast<std::size_t>(which_);
}

ListItem* List::current() const { return item(which_); }

// Numer bieżącego wiersza ustawia się **przez ognisko**, gdy lista je trzyma; w innym razie zostaje
// zapamiętany i użyty przy najbliższym wejściu w listę. Dzięki temu wyjście Tabem na przycisk
// i powrót wracają na ten sam wiersz.
void List::aim(int given) {
    const int much = static_cast<int>(children().many());

    if (much == 0) {
        which_ = -1;

        return;
    }

    which_ = std::clamp(given, 0, much - 1);
    follow_ = true;

    const Root* top = root();

    if (top == nullptr) return;

    for (const Node* at = top->focused_node(); at != nullptr; at = at->parent())
        if (at == this) {
            if (ListItem* one = item(which_); one != nullptr && one->enabled()) one->focus();

            return;
        }
}

// Po numerze — do budowy listy, gdy wołający ma numer opcji, a nie jeszcze wiersz. Ponad ostatni
// przycina do ostatniego, jak dotąd.
void List::which(std::size_t given) {
    aim(static_cast<int>(std::min<std::size_t>(given, static_cast<std::size_t>(INT_MAX))));
}

// Wiersz spoza listy nic nie zmienia.
void List::current(ListItem& one) {
    if (const std::optional<std::size_t> at = children().find(one)) which(*at);
}

std::size_t List::first() const { return static_cast<std::size_t>(shift_); }

void List::pick() {
    if (ListItem* one = item(which_)) one->pick();
}

// Przewinięcie widoku, bez ruszania bieżącego wiersza. Górnej granicy tu nie sprawdzamy, bo zależy
// od wysokości, której w tej chwili nie znamy — przytnie ją malowanie.
//
// **Zawiadamiamy zdarzeniem, nie własnym wywołaniem zwrotnym.** Wcześniej stało tu moved, czyli
// drugi mechanizm obok `on` i `off` — a jedna rzecz ma mieć jedną drogę. Zdarzenie nie niesie
// numeru wiersza, bo pytający ma go pod ręką w `which()`, a widok w `first()`.
void List::first(std::size_t given) {
    const int was = shift_;

    shift_ = static_cast<int>(std::min<std::size_t>(given, static_cast<std::size_t>(INT_MAX)));

    if (shift_ != was) (void)trigger(ScrollEvent{});
}

bool List::going(int to) {
    if (to < 0) return true;

    if (ListItem* one = item(to)) one->focus();

    return true;
}

// Krok o zadaną liczbę wierszy, pozycja po pozycji, żeby niedostępne nie liczyły się jako przebyte
// i żeby ruch zatrzymał się na końcu zamiast wyjść poza.
bool List::walking(int step, int times) {
    int at = which_;

    for (int which = 0; which < times; ++which) {
        const int to = nearest(at, step);

        if (to < 0) break;

        at = to;
    }

    return going(at);
}

// Strzałki i klawisze stron należą do listy; Enter zabrała już pozycja, bo to ona ma ognisko.
// Litery, Tab i Esc oddajemy oknu — odwrotnie niż w polu, gdzie litery trzeba połknąć, bo w polu
// się pisze. Tu litera ma dojść do przycisku w dialogu, a w programie plikowym do szybkiego
// szukania, które panel dopisze sobie sam.
//
// Wysokość strony bierzemy z prostokąta **z poprzedniej klatki**: tylko on zna szerokość i wysokość,
// które lista naprawdę dostała. Przed pierwszym malowaniem strona to jeden wiersz.
bool List::handle(KeyEvent& what) {
    if (what.mods().ctrl || what.mods().alt) return false;

    const int page = std::max(area().size.high, 1);

    switch (what.key().code()) {
        case Code::up: return walking(-1, 1);

        case Code::down: return walking(1, 1);

        case Code::pageup: return walking(-1, page);

        case Code::pagedown: return walking(1, page);

        case Code::home: return going(nearest(-1, 1));

        case Code::end: return going(nearest(static_cast<int>(children().many()), -1));

        default: return false;
    }
}

// Kółko przesuwa **sam widok**, a bieżący wiersz zostawia tam, gdzie stoi — tak samo jak przewijak
// i jak panel Midnight Commandera. Gdyby przesuwało wiersz, każdy ząbek wołałby `moved`, a w
// programie plikowym wisi na tym odświeżanie podglądu.
//
// Na końcu przewijania oddajemy false, więc ruch idzie w górę i przewijak nad listą go dostaje.
bool List::handle(WheelEvent& what) {
    const int much = static_cast<int>(children().many());
    const int window = std::max(area().size.high, 0);
    const int was = shift_;

    shift_ = std::clamp(shift_ - what.steps() * stepping, 0, std::max(much - window, 0));

    if (shift_ == was) return false;

    (void)trigger(ScrollEvent{});

    return true;
}

// Widok na nowe miejsce, przycięte do treści; zawiadomienie tylko o zmianie, jak przy kółku.
void List::slid(int to) {
    const int much = static_cast<int>(children().many());
    const int window = std::max(area().size.high, 0);
    const int was = shift_;

    shift_ = std::clamp(to, 0, std::max(much - window, 0));

    if (shift_ != was) (void)trigger(ScrollEvent{});
}

// **Mysz na pasku** (U6 w twa): strzałka — wiersz, tor — strona, suwak — ciągnięcie. Jak kółko:
// rusza widok, bieżącego wiersza nie.
bool List::handle(MouseEvent& what) {
    const Asked asked = grip_->mouse(what, local(what.where()), *this);
    const int window = std::max(area().size.high, 1);

    switch (asked.part) {
        case Part::none: return false;
        case Part::back: slid(shift_ - 1); break;
        case Part::forth: slid(shift_ + 1); break;
        case Part::before: slid(shift_ - window); break;
        case Part::after: slid(shift_ + window); break;
        case Part::thumb:
            if (asked.to.has_value()) slid(static_cast<int>(*asked.to));
            break;
    }

    return true;
}

// **Tu jest cała księgowość bieżącego wiersza.** Ognisko postawione na pozycji — klawiszem, klikiem,
// Tabem czy z programu — bąbelkuje stamtąd do listy, więc wszystkie te drogi schodzą się w jednym
// miejscu, zamiast każda ustawiać numer po swojemu.
bool List::handle(FocusEvent& what) {
    Node* one = dynamic_cast<Node*>(what.target());

    if (one == nullptr || one == this) return false;

    for (int at = 0; at < static_cast<int>(children().many()); ++at) {
        if (children()[static_cast<std::size_t>(at)].get() != one) continue;

        follow_ = true;

        if (which_ != at) {
            which_ = at;

            (void)trigger(ScrollEvent{});
        }

        break;
    }

    return false;
}

View List::paint(Size given, Area seen) {
    View made{seen.size};

    // Przezroczysty tam, gdzie nic nie rysuje — reguła opisana przy `Node::paint`.

    made.clear({{0, 0}, seen.size});

    const int much = static_cast<int>(children().many());

    // Pozycje mogły zniknąć za plecami listy — `Node::drop` nie jest wirtualne, więc numer wiersza
    // sam się nie naprawi. Przycinamy go tutaj, bo tu i tak liczymy wszystko od nowa.
    if (which_ >= much) which_ = much - 1;

    // Pasek „gdy trzeba" (`needed`) stoi tylko przy liście dłuższej niż okno.
    const int aside = shape_.scrollbar && given.wide > 0 && (!shape_.needed || much > given.high) ? 1 : 0;
    const int wide = given.wide - aside;

    // **Przewinięcie liczy się tutaj**, bo dopiero tu znana jest wysokość, którą lista naprawdę
    // dostała. Doganiamy bieżący wiersz tylko wtedy, gdy się ruszył: bez tego warunku kółko byłoby
    // cofane w tej samej klatce, w której je obrócono.
    if (follow_ && which_ >= 0 && given.high > 0) {
        if (which_ < shift_) shift_ = which_;

        if (which_ >= shift_ + given.high) shift_ = which_ - given.high + 1;
    }

    follow_ = false;

    shift_ = std::clamp(shift_, 0, std::max(much - given.high, 0));

    // Nikt nie wypada z pętli, nawet gdy jest poza widokiem: wiersz niewidoczny dostaje prostokąt
    // o zerowej wysokości i dzięki temu jego `area()` mówi prawdę o tej klatce. Inaczej wiersz,
    // który właśnie zjechał, łapałby myszą w miejscu, gdzie stał poprzednio. Ta sama reguła co
    // w kolumnie; gdyby kiedyś zaczęła kosztować, zerować trzeba tylko te, które właśnie zniknęły.
    for (int at = 0; at < much; ++at) {
        const int y = at - shift_;
        const bool shown = y >= 0 && y < given.high;

        const Area where{{0, shown ? y : 0}, {wide, shown ? 1 : 0}};
        const Area part = Area::crossed(seen, where);

        made.draw(children()[static_cast<std::size_t>(at)]->render(where, part - where.at),
                    part.at - seen.at);
    }

    if (aside != 0) {
        const Scrollbar bar{.at = Scrollbar::edge(given, seen, true),
                            .much = given.high,
                            .shift = static_cast<std::size_t>(shift_),
                            .whole = static_cast<std::size_t>(much),
                            .window = static_cast<std::size_t>(given.high),
                            .skin = skin(),
                            .role = scrollbar_role_};

        bar.draw(made);

        Track mine = bar.track();

        mine.at = Scrollbar::edge(given, {{0, 0}, given}, true);

        grip_->laid(mine);
    } else {
        grip_->laid(std::nullopt);
    }

    return made;
}

}
