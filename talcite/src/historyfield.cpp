#include "talcite/historyfield.hpp"

#include <algorithm>
#include <utility>

#include "popup.hpp"

namespace talcite::ui {

// **Żadnego domyślnego skrótu.** Pole stało wcześniej na Alt+H przypisanym tutaj, zanim wołający
// cokolwiek powiedział — a biblioteka nie ma prawa zajmować kombinacji, o której nikt jej nie
// mówił. Kto chce rozwijanie z klawiatury, dokłada klawisz przez `keys()`; strzałki chodzą po
// historii i bez tego, bo to ruch po własnej treści, a nie czynność.
HistoryField::HistoryField(int wide, std::string text)
    : KeyedNode<Field>(wide, std::move(text)), popup_(std::make_unique<Popup>(*this)) {}

HistoryField::~HistoryField() = default;

// Czynność klawisza aktywnego: rozwinięcie listy. Tak samo `activate()` w pozycji menu naciska,
// w polu legendy wybiera, a w pasku menu przełącza pasek.
void HistoryField::activate() { open(); }

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
void HistoryField::settle() {
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

View HistoryField::paint(Size given, Area seen) {
    settle();

    return Field::paint(given, seen);
}

void HistoryField::clear() {
    close();

    options_.clear();

    seen_ = 0;
    walked_ = 0;
    typed_.clear();
}

// Historia bez dna rośnie do końca programu, a przy zapisie do pliku rośnie też plik. Starsze
// wpisy wypadają od końca, bo to one są najmniej potrzebne.
void HistoryField::keeps(int given) {
    keeps_ = std::max(given, 1);

    if (static_cast<int>(options_.many()) > keeps_) {
        std::vector<std::string> these = options_.all();

        these.resize(static_cast<std::size_t>(keeps_));

        options_.all(std::move(these));
    }

    walked_ = std::min(walked_, options_.many());
}

// Rozwinięta lista mieszka w `Popup` — ta sama, co w wyborze.
int HistoryField::rows() const { return popup_->rows(); }

void HistoryField::rows(int given) { popup_->rows(given); }

bool HistoryField::opened() const { return popup_->opened(); }

void HistoryField::close() { popup_->close(); }

void HistoryField::open() {
    settle();

    if (!enabled()) return;

    // Numer wiersza to numer wpisu we własnej tablicy.
    popup_->open(options_.all(), walked_, [this](std::size_t at) { took(at); });
}

// Jedyne miejsce, w którym wpis z przeszłości trafia do pola — i dlatego jedyne, które o tym
// zawiadamia. Strzałką czy z listy, bez różnicy: to jest ta sama czynność.
//
// `Field::text` sam stawia kursor na końcu, więc po wzięciu wpisu można od razu dopisywać.
void HistoryField::took(std::size_t which) {
    if (which >= options_.many()) return;

    // Zapamiętujemy to, co użytkownik pisał, **zanim** nadpiszemy pole. Tylko przy wejściu
    // w przeszłość: kolejne kroki mają wracać do tego samego, a nie do poprzedniego wpisu.
    if (walked_ >= options_.many()) typed_ = text();

    walked_ = which;

    text(options_[which]);

    (void)trigger(PickEvent{});
}

// Powrót do tego, co użytkownik pisał sam. **Nie zawiadamia**: nic z przeszłości do pola nie
// trafiło, wręcz przeciwnie. Bez tego jednego kroku historia jest pułapką — raz wszedłeś i nie ma
// jak wrócić do swojego wiersza.
void HistoryField::back() {
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
bool HistoryField::handle(KeyEvent& what) {
    settle();

    if (!enabled() || options_.empty()) return Field::handle(what);

    const Mods mods = what.mods();

    // **Klawisz rozwijający leży w zbiorze klawiszy**, jak w każdym innym widżecie. Składanie
    // wielkości liter, dokładne Ctrl i Alt oraz przymknięte Shift — wszystko to robi `contains`,
    // więc nie ma tu już własnej reguły dopasowania.
    if (keys().contains(what.key())) {
        activate();

        return true;
    }

    if (mods.ctrl || mods.alt) return Field::handle(what);

    const std::size_t many = options_.many();

    switch (what.key().code()) {
        case Code::up:
            // Z „nigdzie" wchodzimy na najświeższy wpis; dalej w górę znaczy dalej w przeszłość.
            if (walked_ >= many) {
                took(0);

                return true;
            }

            if (walked_ + 1 >= many) return Field::handle(what);

            took(walked_ + 1);

            return true;

        case Code::down:
            if (walked_ >= many) return Field::handle(what);

            if (walked_ == 0) {
                back();

                return true;
            }

            took(walked_ - 1);

            return true;

        default: return Field::handle(what);
    }
}

// **Gasnąca historia zwija swoją listę** — ta sama reguła co w wyborze i z tego samego powodu:
// wyłączone nie wybiera, a rozwinięta lista wisiałaby nad polem, które już nie odpowiada. Ognisko
// wraca przy zamknięciu do najbliższego chętnego przodka, bo historia już go nie bierze. Zdarzenia
// nie bierzemy: zwinięcie to porządek po sobie, a przodek słuchający wyłączeń ma je dostać.
bool HistoryField::handle(DisableEvent&) {
    close();

    return false;
}

}
