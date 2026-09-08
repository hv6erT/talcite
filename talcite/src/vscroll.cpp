#include "talcite/vscroll.hpp"

#include "talcite/bar.hpp"
#include "talcite/skin.hpp"

#include <algorithm>

namespace talcite::ui {

namespace {

// Trzy wiersze na ząbek kółka — tyle daje większość terminali i tyle wyrabia oko.
constexpr int stepping = 3;

}

// **Zawiadamiamy tylko o zmianie i tylko stąd.** Przycięcie do bieżącego okna dzieje się
// w `paint`, więc nadawanie z każdej zmiany `shift_` znaczyłoby podmienianie drzewa w trakcie
// składania klatki. Skutek jest znany i przyjęty: zwężenie okna, które samo dosunie zawartość,
// nie zawiadomi nikogo.
void VScroll::shift(int given) {
    const int want = std::max(given, 0);

    if (want == shift_) return;

    shift_ = want;

    ScrollEvent moved;

    (void)trigger(moved);
}

// Dziecko pytamy o wysokość bez ograniczenia, bo po to jest przewijanie. Oddajemy jednak co
// najwyżej tyle, ile sami dostaliśmy: rodzic nie ma skąd wziąć więcej.
Size VScroll::wants(Size room) const {
    if (children().empty()) return {};

    const int aside = style_.progress && room.wide > 0 ? 1 : 0;
    const Size wants = children().front()->wants({room.wide - aside, plenty});

    return {std::min(wants.wide + aside, room.wide), std::min(wants.high, room.high)};
}

// Dziecko stoi u nas w prostokącie przesuniętym w górę o przewinięcie i tak wysokim, jak samo
// chciało. Widoczna jest z tego tylko część wspólna z naszym oknem — i tylko tę część dziecko
// w ogóle narysuje, bo dostaje ją jako widoczność.
View VScroll::paint(Size given, Area seen) {
    View made{seen.size};

    // Przezroczysty tam, gdzie nic nie rysuje — reguła opisana przy `Node::paint`.

    made.bare({{0, 0}, seen.size});

    if (children().empty()) return made;

    Node& one = *children().front();

    // Pasek zabiera jedną kolumnę z prawej, więc treść dostaje o tyle mniej miejsca.
    const int aside = style_.progress && given.wide > 0 ? 1 : 0;
    const int wide = given.wide - aside;

    whole_ = std::max(one.wants({wide, plenty}).high, 0);
    window_ = given.high;

    // Treść mogła się skrócić między klatkami — wtedy przewinięcie wisiałoby za jej końcem.
    shift_ = std::clamp(shift_, 0, std::max(whole_ - window_, 0));

    const Area where{{0, -shift_}, {wide, whole_}};
    const Area part = Area::crossed(seen, where);

    made.within(one.render(where, part - where.at), part.at - seen.at);

    if (aside != 0)
        edged(made, given, seen, true, given.high, static_cast<std::size_t>(shift_),
              static_cast<std::size_t>(whole_), static_cast<std::size_t>(window_), skin(), bar_);

    return made;
}

// Na końcu przewijania oddajemy false, więc zdarzenie idzie w górę — przewijak w przewijaku
// przekazuje ruch dalej, zamiast go połykać.
bool VScroll::react(WheelEvent& what) {
    const int was = shift_;

    shift_ = std::clamp(shift_ - what.steps() * stepping, 0, std::max(whole_ - window_, 0));

    if (shift_ == was) return false;

    ScrollEvent moved;

    (void)trigger(moved);

    return true;
}

}
