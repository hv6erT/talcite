#include "talcite/handler.hpp"

#include <algorithm>
#include <unordered_map>

namespace talcite::ui {

// Uchwyty leżą w mapie z typu zdarzenia na wektor par numer-funkcja. Funkcję trzymamy przez
// wskaźnik do niczego, bo mapa nie może znać typów, których jeszcze nie ma; klucz mapy jest
// jednak samym typem, więc odzyskanie po drugiej stronie nie jest zgadywaniem.
struct Handler::Hands {
    std::unordered_map<std::type_index, std::vector<std::pair<std::uint64_t, std::shared_ptr<void>>>>
        held;
    std::uint64_t next = 0;
};

Handler::Handler() = default;

// W ciele, nie przy deklaracji: Hands jest w nagłówku niepełne, a unique_ptr musi znać rozmiar
// tego, co zwalnia.
Handler::~Handler() = default;

Handler* Handler::parent() const { return nullptr; }

// **Kanał widżetu zna tylko zdarzenia biblioteki**, po jednym `handle` na typ. Zdarzenie programu
// (`QuitEvent` z twa) idzie wyłącznie kanałem wołających, przez `on<>` — `carried` sprawdza
// w czasie kompilacji, czy jest dla niego `handle`, i gdy nie ma, kanału widżetu nie otwiera.
// Dawniej łapało je ogólne `react(Event&)`, które zawsze oddawało fałsz i którego nikt nie
// przesłaniał, a przy okazji po cichu połykało typ **wywiedziony** ze zdarzenia biblioteki bez
// własnego przeciążenia. Teraz taki typ się nie kompiluje: `known` mówi, że przeciążenie pasuje,
// `exact`, że nie dokładnie, i `static_assert` stawia sprawę jasno.
//
// Nazwa `handle`, a nie dawne `react`: klasa nazywa się `Handler`, więc „obsługuje”; wołający
// mówi `on`, widżet `handle`. To ten sam rodowód, co `handleEvent` w Turbo Vision.
bool Handler::handle(KeyEvent&) { return false; }

bool Handler::handle(MouseEvent&) { return false; }

bool Handler::handle(ClickEvent&) { return false; }

bool Handler::handle(DoubleClickEvent&) { return false; }

bool Handler::handle(WheelEvent&) { return false; }

bool Handler::handle(SizeEvent&) { return false; }

bool Handler::handle(FocusEvent&) { return false; }

bool Handler::handle(UnfocusEvent&) { return false; }

bool Handler::handle(EnableEvent&) { return false; }

bool Handler::handle(DisableEvent&) { return false; }

bool Handler::handle(ScrollEvent&) { return false; }

bool Handler::handle(PickEvent&) { return false; }

bool Handler::handle(CloseEvent&) { return false; }

bool Handler::handle(ShortcutEvent&) { return false; }

bool Handler::handle(PasteEvent&) { return false; }

// Przydział dopiero przy pierwszym dopięciu. Węzeł, do którego nikt nic nie dopiął — a takich
// jest większość — kosztuje jeden wskaźnik, a nie mapę. Numery idą od jedynki, więc zero może
// znaczyć pusty uchwyt.
Hook Handler::kept(std::type_index which, std::shared_ptr<void> what) {
    if (!hands_) hands_ = std::make_unique<Hands>();

    const std::uint64_t one = ++hands_->next;

    hands_->held[which].emplace_back(one, std::move(what));

    return Hook{which, one};
}

// **Uchwyt zna swój typ**, więc odpięcie go nie potrzebuje — dawniej trzeba było napisać
// `off<KeyEvent>(numer)` i pomyłka w typie po cichu nie odpinała niczego. Po odpięciu uchwyt jest
// pusty, więc `empty()` znaczy dokładnie „nic przez niego nie wisi”; odpięcie pustego nic nie
// robi. Uchwyt nie odpina się sam przy zniszczeniu: większość wołających go odrzuca, bo uchwyt
// ma żyć tyle, co węzeł.
void Handler::off(Hook& which) {
    const Hook gone = std::exchange(which, Hook{});

    if (!hands_ || gone.empty()) return;

    const auto at = hands_->held.find(gone.type_);
    if (at == hands_->held.end()) return;

    std::vector<std::pair<std::uint64_t, std::shared_ptr<void>>>& mine = at->second;

    mine.erase(std::remove_if(mine.begin(), mine.end(),
                              [&gone](const std::pair<std::uint64_t, std::shared_ptr<void>>& kept) {
                                  return kept.first == gone.number_;
                              }),
               mine.end());

    if (mine.empty()) hands_->held.erase(at);
}

// Oddajemy kopię, nie wgląd. Uchwyt ma prawo w trakcie własnego wywołania dopiąć albo odpiąć
// inny — a to przestawiłoby wektor pod nogami pętli. Pusty wektor nic nie kosztuje, więc płaci
// za to tylko ten węzeł, który uchwyty naprawdę ma.
std::vector<std::shared_ptr<void>> Handler::among(std::type_index which) const {
    std::vector<std::shared_ptr<void>> made;

    if (!hands_) return made;

    const auto at = hands_->held.find(which);
    if (at == hands_->held.end()) return made;

    made.reserve(at->second.size());

    for (const std::pair<std::uint64_t, std::shared_ptr<void>>& one : at->second)
        made.push_back(one.second);

    return made;
}

}
