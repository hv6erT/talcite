#include "talcite/select.hpp"

#include <algorithm>
#include <utility>

#include "talcite/encoding.hpp"
#include "talcite/list.hpp"
#include "talcite/root.hpp"
#include "talcite/skin.hpp"

#include "eliding.hpp"
#include "rolling.hpp"

namespace talcite::ui {

namespace {

// Spacja przed znakiem rozwinięcia i sam znak. Dwie kratki, zawsze — napis ustępuje im pierwszy,
// bo to znak rozwinięcia czyni z tego wiersza wybór, a nie zwykły tekst.
constexpr int aside = 2;

}

Select::Select() : OptionsNode("talcite.select") {}

Select::Select(std::vector<std::string> options, std::size_t which) : Select() {
    this->options().all(std::move(options));

    at(which);
}

void Select::most(int given) { most_ = std::max(given, 1); }

// **Rozwinięta lista należy do okna, nie do nas.** Ono ją trzyma, ono ją zamyka Escapem, ono zwraca
// ognisko. Trzymając ją mocno, zostawalibyśmy z martwym uchwytem po każdym zamknięciu nie naszą
// ręką; słabe wskazanie mówi dokładnie tyle, ile jest prawdą — „ta jest moja, dopóki istnieje".
bool Select::opened() const { return !rolled_.expired(); }

void Select::close() {
    if (const std::shared_ptr<Veil> one = rolled_.lock()) one->close();

    rolled_.reset();
}

void Select::open() {
    if (!able() || options().empty() || opened()) return;

    const Rolls made = rolling(*this, options().all(), at(), most_);

    if (!made.list) return;

    // **Wybór wraca zwykłym zdarzeniem, nie wywołaniem zwrotnym.** `ListItem::pick()` ogłasza
    // `PickEvent`, a my słuchamy go na liście, którą sami zamówiliśmy — bo tylko my wiemy, co z nim
    // zrobić. `Rolled` nie bierze w tym udziału i nie musi wiedzieć, po co ją niesie.
    //
    // Uchwyt trzyma **słabe** wskazanie na nas: rozwinięta lista może przeżyć widżet, jeśli ktoś
    // zdejmie go z drzewa przy otwartej liście.
    const std::weak_ptr<Node> mine = weak_from_this();
    const std::weak_ptr<List> theirs = made.list;

    // **Wybór wraca wartością, a nie numerem wiersza.** Lista powstała przy otwarciu, a numer
    // rozwiązałby się dopiero przy naciśnięciu — więc gdyby ktoś w międzyczasie zmienił opcje,
    // do pola trafiłby inny napis, niż widać było. Migawka napisów siedzi w domknięciu, a napisy
    // są jednoznaczne, bo zbiór nie dopuszcza powtórek.
    (void)made.list->on<PickEvent>([mine, theirs, these = options().all()](PickEvent&) {
        const std::shared_ptr<Node> kept = mine.lock();
        const std::shared_ptr<List> from = theirs.lock();

        if (!kept || !from) return true;

        auto& me = static_cast<Select&>(*kept);

        const int at = from->which();

        me.close();

        if (at >= 0 && static_cast<std::size_t>(at) < these.size())
            me.chose(these[static_cast<std::size_t>(at)]);

        return true;
    });

    rolled_ = made.veil;
}

// Wyłączony wybór nie może zostać z ogniskiem: Tab przestaje go widzieć, więc ognisko utknęłoby na
// czymś, z czego nie da się wyjść. Rozwiniętą listę przy okazji zamykamy — wyłączone nie wybiera.
// Gasnący wybór najpierw zwija listę, a dopiero potem oddaje ognisko — reszta jest wspólna
// z grupą opcji i mieszka w `OptionsNode`.
void Select::able(bool given) {
    if (!given) close();

    OptionsNode::able(given);
}

// Pusty wybór **dalej bierze ognisko**, choć pusta grupa opcji go nie bierze. Różnica nie jest
// kaprysem: pusta grupa zajmuje zero wierszy i nie widać jej wcale, a pusty wybór zajmuje wiersz
// i widać go razem ze znakiem rozwinięcia. Wędrówka, która omija coś widocznego, wygląda na
// zepsutą.
bool Select::focusable() const { return able(); }

Size Select::wants(Size room) const {
    int wide = 0;

    for (const std::string& one : options()) wide = std::max(wide, measured(one).wide);

    return {std::min(wide + aside, room.wide), std::min(1, room.high)};
}

void Select::chose(const std::string& given) {
    const int made = options().which(given);

    if (made < 0 || static_cast<std::size_t>(made) == at()) return;

    at(static_cast<std::size_t>(made));

    PickEvent one;

    (void)trigger(one);
}

View Select::paint(Size given, Area seen) {
    View made{seen.size};

    // Przezroczysty tam, gdzie nic nie rysuje — ta sama reguła i ten sam powód co w przycisku.
    made.bare({{0, 0}, seen.size});

    const Skin& how = skin();

    // Rodzina ról na stan, złożona z rdzenia i końcówki — ta sama umowa co w przycisku i w polu
    // wyboru, żeby autor skórki opisywał `<rdzeń>`, `<rdzeń>.focus`, `<rdzeń>.off` i niczego nie
    // zgadywał.
    const Style look = how.get<Style>(!able() ? role() + ".off" : focused() ? role() + ".focus" : role());

    made.fill({{-seen.at.x, -seen.at.y}, {given.wide, 1}}, look);

    if (given.wide <= 0) return made;

    // Znak rozwinięcia stoi przy **prawej krawędzi**, cokolwiek by nam dano — tak jak w oknie
    // ustawień, gdzie wybór ciągnie się przez całą szerokość kolumny.
    made.letter({given.wide - 1 - seen.at.x, -seen.at.y}, how.get<char32_t>(role() + ".mark"), look);

    const int room = std::max(given.wide - aside, 0);

    if (room <= 0) return made;

    made.string({-seen.at.x, -seen.at.y},
                elided(which().value_or(std::string{}), room, ElideStyle::right,
                       how.get<char32_t>("talcite.elide")),
                look);

    return made;
}

// Enter i spacja rozwijają. Strzałki przestawiają wartość **bez rozwijania** i zawiadamiają — tak
// samo jak w grupie opcji, gdzie już to ustaliliśmy.
//
// Na krańcu strzałka **idzie wyżej**, żeby w oknie przeszła do następnego widżetu zamiast utknąć.
// Litery, Tab i F-klawisze oddajemy zawsze: gdyby wybór je łykał, przycisk „Anuluj" przestałby
// odpowiadać na swoją literę, ilekroć ognisko stoi tutaj.
bool Select::react(KeyEvent& what) {
    if (!able() || options().empty()) return false;

    if (what.mods().ctrl || what.mods().alt) return false;

    const Code code = what.key().code();

    // Sam Enter, bez spacji — tak samo jak w przycisku i z tego samego powodu.
    if (code == Code::enter) {
        open();

        return true;
    }

    const std::size_t last = options().many() - 1;

    switch (code) {
        case Code::up:
            if (at() == 0) return false;

            chose(options()[at() - 1]);

            return true;

        case Code::down:
            if (at() == last) return false;

            chose(options()[at() + 1]);

            return true;

        case Code::home:
            if (at() == 0) return false;

            chose(options()[0]);

            return true;

        case Code::end:
            if (at() == last) return false;

            chose(options()[last]);

            return true;

        default: return false;
    }
}

bool Select::react(ClickEvent&) {
    if (!able()) return true;

    if (root() != nullptr) focus();

    open();

    return true;
}

}
