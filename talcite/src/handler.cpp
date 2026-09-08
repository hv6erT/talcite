#include "talcite/handler.hpp"

#include <algorithm>
#include <unordered_map>

namespace talcite::ui {

// Uchwyty leżą w mapie z typu zdarzenia na wektor par numer-funkcja. Funkcję trzymamy przez
// wskaźnik do niczego, bo mapa nie może znać typów, których jeszcze nie ma; klucz mapy jest
// jednak samym typem, więc odzyskanie po drugiej stronie nie jest zgadywaniem.
struct Handler::Hands {
    std::unordered_map<std::type_index, std::vector<std::pair<Handle, std::shared_ptr<void>>>>
        held;
    Handle next = 0;
};

Handler::Handler() = default;

// W ciele, nie przy deklaracji: Hands jest w nagłówku niepełne, a unique_ptr musi znać rozmiar
// tego, co zwalnia.
Handler::~Handler() = default;

Handler* Handler::parent() const { return nullptr; }

bool Handler::react(KeyEvent&) { return false; }

bool Handler::react(MouseEvent&) { return false; }

bool Handler::react(ClickEvent&) { return false; }

bool Handler::react(DoubleClickEvent&) { return false; }

bool Handler::react(WheelEvent&) { return false; }

bool Handler::react(SizeEvent&) { return false; }

bool Handler::react(FocusEvent&) { return false; }

bool Handler::react(UnfocusEvent&) { return false; }

bool Handler::react(ScrollEvent&) { return false; }

bool Handler::react(PickEvent&) { return false; }

bool Handler::react(CloseEvent&) { return false; }

bool Handler::react(HotEvent<KeyEvent>&) { return false; }

bool Handler::react(PasteEvent&) { return false; }

// Przydział dopiero przy pierwszym dopięciu. Węzeł, do którego nikt nic nie dopiął — a takich
// jest większość — kosztuje jeden wskaźnik, a nie mapę.
Handle Handler::kept(std::type_index which, std::shared_ptr<void> what) {
    if (!hands_) hands_ = std::make_unique<Hands>();

    const Handle one = ++hands_->next;

    hands_->held[which].emplace_back(one, std::move(what));

    return one;
}

void Handler::freed(std::type_index which, Handle one) {
    if (!hands_ || one == 0) return;

    const auto at = hands_->held.find(which);
    if (at == hands_->held.end()) return;

    std::vector<std::pair<Handle, std::shared_ptr<void>>>& mine = at->second;

    mine.erase(std::remove_if(mine.begin(), mine.end(),
                              [one](const std::pair<Handle, std::shared_ptr<void>>& kept) {
                                  return kept.first == one;
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

    for (const std::pair<Handle, std::shared_ptr<void>>& one : at->second)
        made.push_back(one.second);

    return made;
}

}
