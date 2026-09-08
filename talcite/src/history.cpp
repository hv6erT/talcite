#include "talcite/history.hpp"

#include <algorithm>
#include <utility>

#include "talcite/list.hpp"

#include "folding.hpp"
#include "rolling.hpp"

namespace talcite::ui {

namespace {

// Litery bez różnicy wielkości, klawisze nazwane na równość — ta sama reguła co w `Keys::contains`.
bool alike(Key given, Key mine) {
    if (given == mine) return true;

    const char32_t here = given.letter();
    const char32_t there = mine.letter();

    return here != 0 && there != 0 && folded(here) == folded(there);
}

}

// Alt+H rozwija listę — tak jak w Midnight Commanderze. To jest wybór aplikacji, nie prawo natury,
// więc `rolls()` pozwala go przestawić; domyślnie stoi tam, gdzie użytkownik MC będzie go szukał.
// **Żadnego domyślnego skrótu.** Pole stało wcześniej na Alt+H przypisanym tutaj, zanim wołający
// cokolwiek powiedział — a biblioteka nie ma prawa zajmować kombinacji, o której nikt jej nie
// mówił. Kto chce rozwijanie z klawiatury, woła `rolls(Key)`; strzałki chodzą po historii i bez
// tego, bo to ruch po własnej treści, a nie czynność.
History::History(int wide, std::string text) : KeyedNode<Field>(wide, std::move(text)) {}

// Czynność klawisza aktywnego: rozwinięcie listy. Tak samo `act()` w pozycji menu naciska,
// w polu legendy wybiera, a w pasku menu przełącza pasek.
void History::act() { open(); }

// Dostępność ma po polu tekstowym, a nie własną: `KeyedNode` daje wprawdzie swoją, ale pole miało
// ją pierwsze i to ono nią rządzi. Bez tego przesłonięcia wyłączona historia dalej odpowiadałaby
// na swój klawisz.
bool History::able() const { return Field::able(); }

// **Zbiór opcji jest publiczny i to przez niego dokłada wołający** — `options().insert(0, one)`
// stawia polecenie na czele, a powtórka wraca na czoło zamiast leżeć dwa razy, bo zbiór nie
// dopuszcza duplikatów. Historia nie ma własnego `add`: byłby drugą nazwą na to samo.
//
// Po sobie sprząta **sama i u siebie**, zamiast kazać się zawiadamiać: przycina do `keeps()`
// i — gdy widzi, że zbiór urósł albo zmalał od ostatniego spojrzenia — kończy wędrówkę po
// przeszłości. Wołane z malowania i z obsługi klawisza, więc między zmianą a pierwszym użyciem
// nie ma szczeliny.
//
// Zmiana, po której zbiór ma tyle samo wpisów, co przedtem, przechodzi niezauważona — kursor
// zostaje, gdzie stał. To jedyny przypadek, w którym sprzątanie nie zadziała, i jest nieszkodliwy:
// wędrówka i tak przycina się do rozmiaru przy każdym kroku.
void History::settle() {
    if (static_cast<int>(options_.many()) > keeps_) {
        std::vector<std::string> these = options_.all();

        these.resize(static_cast<std::size_t>(keeps_));

        options_.all(std::move(these));
    }

    if (options_.many() == seen_) return;

    seen_ = options_.many();
    walked_ = options_.many();
    typed_.clear();
}

View History::paint(Size given, Area seen) {
    settle();

    return Field::paint(given, seen);
}

void History::clear() {
    close();

    options_.clear();

    seen_ = 0;
    walked_ = 0;
    typed_.clear();
}

// Historia bez dna rośnie do końca programu, a przy zapisie do pliku rośnie też plik. Starsze
// wpisy wypadają od końca, bo to one są najmniej potrzebne.
void History::keeps(int given) {
    keeps_ = std::max(given, 1);

    if (static_cast<int>(options_.many()) > keeps_) {
        std::vector<std::string> these = options_.all();

        these.resize(static_cast<std::size_t>(keeps_));

        options_.all(std::move(these));
    }

    walked_ = std::min(walked_, options_.many());
}

void History::most(int given) { most_ = std::max(given, 1); }

// **Rozwinięta lista należy do okna, nie do nas** — ta sama reguła i ten sam powód co w wyborze.
bool History::opened() const { return !rolled_.expired(); }

void History::close() {
    if (const std::shared_ptr<Veil> one = rolled_.lock()) one->close();

    rolled_.reset();
}

void History::open() {
    settle();

    if (!able() || options_.empty() || opened()) return;

    const Rolls made = rolling(*this, options_.all(), walked_, most_);

    if (!made.list) return;

    const std::weak_ptr<Node> mine = weak_from_this();
    const std::weak_ptr<List> theirs = made.list;

    (void)made.list->on<PickEvent>([mine, theirs](PickEvent&) {
        const std::shared_ptr<Node> kept = mine.lock();
        const std::shared_ptr<List> from = theirs.lock();

        if (!kept || !from) return true;

        auto& me = static_cast<History&>(*kept);

        const int at = from->which();

        me.close();

        if (at >= 0) me.took(static_cast<std::size_t>(at));

        return true;
    });

    rolled_ = made.veil;
}

// Jedyne miejsce, w którym wpis z przeszłości trafia do pola — i dlatego jedyne, które o tym
// zawiadamia. Strzałką czy z listy, bez różnicy: to jest ta sama czynność.
//
// `Field::text` sam stawia kursor na końcu, więc po wzięciu wpisu można od razu dopisywać.
void History::took(std::size_t which) {
    if (which >= options_.many()) return;

    // Zapamiętujemy to, co użytkownik pisał, **zanim** nadpiszemy pole. Tylko przy wejściu
    // w przeszłość: kolejne kroki mają wracać do tego samego, a nie do poprzedniego wpisu.
    if (walked_ >= options_.many()) typed_ = text();

    walked_ = which;

    text(options_[which]);

    PickEvent one;

    (void)trigger(one);
}

// Powrót do tego, co użytkownik pisał sam. **Nie zawiadamia**: nic z przeszłości do pola nie
// trafiło, wręcz przeciwnie. Bez tego jednego kroku historia jest pułapką — raz wszedłeś i nie ma
// jak wrócić do swojego wiersza.
void History::back() {
    if (walked_ >= options_.many()) return;

    walked_ = options_.many();

    text(typed_);

    typed_.clear();
}

// Góra idzie w przeszłość, dół z powrotem. Klawisz rozwijający otwiera listę. Wszystko inne —
// litery, Tab, Home, End, kasowanie — spada do pola i działa dokładnie jak dotąd.
//
// Pusta historia nie bierze **niczego**: nie ma po czym chodzić, więc klawisz ma iść wyżej, zamiast
// zginąć. Tak samo na krańcu wędrówki.
bool History::react(KeyEvent& what) {
    settle();

    if (!able() || options_.empty()) return Field::react(what);

    const Mods mods = what.mods();

    // **Klawisz rozwijający leży w zbiorze klawiszy**, jak w każdym innym widżecie. Składanie
    // wielkości liter, dokładne Ctrl i Alt oraz przymknięte Shift — wszystko to robi `contains`,
    // więc nie ma tu już własnej reguły dopasowania.
    if (keys().contains(what.key())) {
        act();

        return true;
    }

    if (mods.ctrl || mods.alt) return Field::react(what);

    const std::size_t many = options_.many();

    switch (what.key().code()) {
        case Code::up:
            // Z „nigdzie" wchodzimy na najświeższy wpis; dalej w górę znaczy dalej w przeszłość.
            if (walked_ >= many) {
                took(0);

                return true;
            }

            if (walked_ + 1 >= many) return Field::react(what);

            took(walked_ + 1);

            return true;

        case Code::down:
            if (walked_ >= many) return Field::react(what);

            if (walked_ == 0) {
                back();

                return true;
            }

            took(walked_ - 1);

            return true;

        default: return Field::react(what);
    }
}

}
