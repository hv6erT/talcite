#include "talcite/select.hpp"

#include <algorithm>
#include <utility>

#include "talcite/encoding.hpp"
#include "talcite/root.hpp"
#include "talcite/skin.hpp"

#include "eliding.hpp"
#include "popup.hpp"

namespace talcite::ui {

namespace {

// Spacja przed znakiem rozwinięcia i sam znak. Dwie kratki, zawsze — napis ustępuje im pierwszy,
// bo to znak rozwinięcia czyni z tego wiersza wybór, a nie zwykły tekst.
constexpr int aside = 2;

}

Select::Select() : OptionsNode("talcite.select"), popup_(std::make_unique<Popup>(*this)) {}

Select::Select(std::vector<std::string> options, std::size_t which) : Select() {
    this->options().all(std::move(options));

    this->which(which);
}

Select::~Select() = default;

// Rozwinięta lista mieszka w `Popup` — tam jest i liczba wierszy, i to, czyja jest zasłona.
int Select::rows() const { return popup_->rows(); }

void Select::rows(int given) { popup_->rows(given); }

bool Select::opened() const { return popup_->opened(); }

void Select::close() { popup_->close(); }

void Select::open() {
    if (!enabled() || options().empty()) return;

    // **Wybór wraca wartością, a nie numerem wiersza.** Lista powstała przy otwarciu, a numer
    // rozwiązałby się dopiero przy naciśnięciu — więc gdyby ktoś w międzyczasie zmienił opcje,
    // do pola trafiłby inny napis, niż widać było. Migawka napisów siedzi w domknięciu, a napisy
    // są jednoznaczne, bo zbiór nie dopuszcza powtórek.
    popup_->open(options().all(), *which(),
                 [this, these = options().all()](std::size_t at) { chose(these[at]); });
}

Size Select::wants(Size room) const {
    int wide = 0;

    for (const std::string& one : options()) wide = std::max(wide, measured(one).wide);

    return {std::min(wide + aside, room.wide), std::min(1, room.high)};
}

void Select::chose(const std::string& given) {
    const std::optional<std::size_t> made = options().find(given);

    if (!made || made == which()) return;

    which(*made);

    (void)trigger(PickEvent{});
}

View Select::paint(Size given, Area seen) {
    View made{seen.size};

    // Przezroczysty tam, gdzie nic nie rysuje — ta sama reguła i ten sam powód co w przycisku.
    made.clear({{0, 0}, seen.size});

    const Skin& how = skin();

    // Rodzina ról na stan, złożona z rdzenia i końcówki — ta sama umowa co w przycisku i w polu
    // wyboru, żeby autor skórki opisywał `<rdzeń>`, `<rdzeń>.focus`, `<rdzeń>.disabled` i niczego
    // nie zgadywał.
    const Style look = how.style(!enabled() ? role() + ".disabled" : focused() ? role() + ".focus" : role());

    made.fill({{-seen.at.x, -seen.at.y}, {given.wide, 1}}, look);

    if (given.wide <= 0) return made;

    // Znak rozwinięcia stoi przy **prawej krawędzi**, cokolwiek by nam dano — tak jak w oknie
    // ustawień, gdzie wybór ciągnie się przez całą szerokość kolumny.
    made.put({given.wide - 1 - seen.at.x, -seen.at.y}, how.glyph(role() + ".mark"), look);

    const int room = std::max(given.wide - aside, 0);

    if (room <= 0) return made;

    made.put({-seen.at.x, -seen.at.y},
                elided(value().value_or(std::string{}), room, Elide::right,
                       how.glyph("talcite.elide")),
                look);

    return made;
}

// Enter i spacja rozwijają. Strzałki przestawiają wartość **bez rozwijania** i zawiadamiają — tak
// samo jak w grupie opcji, gdzie już to ustaliliśmy.
//
// Na krańcu strzałka **idzie wyżej**, żeby w oknie przeszła do następnego widżetu zamiast utknąć.
// Litery, Tab i F-klawisze oddajemy zawsze: gdyby wybór je łykał, przycisk „Anuluj" przestałby
// odpowiadać na swoją literę, ilekroć ognisko stoi tutaj.
bool Select::handle(KeyEvent& what) {
    if (!enabled() || options().empty()) return false;

    if (what.mods().ctrl || what.mods().alt) return false;

    const Code code = what.key().code();

    // Sam Enter, bez spacji — tak samo jak w przycisku i z tego samego powodu.
    if (code == Code::enter) {
        open();

        return true;
    }

    const std::size_t last = options().many() - 1;
    const std::size_t at = *which();

    switch (code) {
        case Code::up:
            if (at == 0) return false;

            chose(options()[at - 1]);

            return true;

        case Code::down:
            if (at == last) return false;

            chose(options()[at + 1]);

            return true;

        case Code::home:
            if (at == 0) return false;

            chose(options()[0]);

            return true;

        case Code::end:
            if (at == last) return false;

            chose(options()[last]);

            return true;

        default: return false;
    }
}

bool Select::handle(ClickEvent&) {
    if (!enabled()) return true;

    if (root() != nullptr) focus();

    open();

    return true;
}

// **Gasnący wybór zwija swoją listę.** Wyłączone nie wybiera, a rozwinięta lista wisiałaby nad
// polem, które już nie odpowiada. Ognisko siedzi wtedy w liście, a zamknięcie zasłony oddaje je
// wyborowi przez `focus()` — który wyłączonego nie weźmie, więc ognisko idzie dalej samo.
//
// Pusty wybór **dalej bierze ognisko**, choć pusta grupa opcji go nie bierze, więc `takes_focus` nie
// jest przesłonięte. Pusta grupa zajmuje zero wierszy i nie widać jej wcale, a pusty wybór zajmuje
// wiersz i widać go razem ze znakiem rozwinięcia. Wędrówka, która omija coś widocznego, wygląda na
// zepsutą.
//
// Zdarzenie bąbelkuje, ale wybór nie ma dzieci, więc każde, które tu dochodzi, jest jego własne.
// Zwinięcie listy to porządek po sobie, a nie odpowiedź na zdarzenie, więc wybór go nie bierze:
// przodek, który słucha wyłączeń w swoim poddrzewie, dostaje i to.
bool Select::handle(DisableEvent&) {
    close();

    return false;
}

}
