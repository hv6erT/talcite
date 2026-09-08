#include "talcite/hscroll.hpp"

#include "talcite/bar.hpp"
#include "talcite/skin.hpp"

#include <algorithm>

namespace talcite::ui {

namespace {

// Kółko w poziomie przesuwa o tyle samo, ile w pionie — inaczej ta sama myszka zachowywałaby się
// inaczej zależnie od kierunku.
constexpr int stepping = 3;

}

// **Zawiadamiamy tylko o zmianie i tylko stąd.** Przycięcie do bieżącego okna dzieje się
// w `paint`, więc nadawanie z każdej zmiany `shift_` znaczyłoby podmienianie drzewa w trakcie
// składania klatki. Skutek jest znany i przyjęty: zwężenie okna, które samo dosunie zawartość,
// nie zawiadomi nikogo.
void HScroll::shift(int given) {
    const int want = std::max(given, 0);

    if (want == shift_) return;

    shift_ = want;

    ScrollEvent moved;

    (void)trigger(moved);
}

Size HScroll::wants(Size room) const {
    if (children().empty()) return {};

    const int aside = style_.progress && room.high > 0 ? 1 : 0;
    const Size wants = children().front()->wants({plenty, room.high - aside});

    return {std::min(wants.wide, room.wide), std::min(wants.high + aside, room.high)};
}

View HScroll::paint(Size given, Area seen) {
    View made{seen.size};

    // Przezroczysty tam, gdzie nic nie rysuje — reguła opisana przy `Node::paint`.

    made.bare({{0, 0}, seen.size});

    if (children().empty()) return made;

    Node& one = *children().front();

    // Pasek zabiera dolny wiersz.
    const int aside = style_.progress && given.high > 0 ? 1 : 0;
    const int high = given.high - aside;

    whole_ = std::max(one.wants({plenty, high}).wide, 0);
    window_ = given.wide;

    shift_ = std::clamp(shift_, 0, std::max(whole_ - window_, 0));

    const Area where{{-shift_, 0}, {whole_, high}};
    const Area part = Area::crossed(seen, where);

    made.within(one.render(where, part - where.at), part.at - seen.at);

    if (aside != 0)
        edged(made, given, seen, false, given.wide, static_cast<std::size_t>(shift_),
              static_cast<std::size_t>(whole_), static_cast<std::size_t>(window_), skin(), bar_);

    return made;
}

bool HScroll::react(WheelEvent& what) {
    const int was = shift_;

    shift_ = std::clamp(shift_ - what.steps() * stepping, 0, std::max(whole_ - window_, 0));

    if (shift_ == was) return false;

    ScrollEvent moved;

    (void)trigger(moved);

    return true;
}

}
