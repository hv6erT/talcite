#include "talcite/scroll.hpp"

#include "talcite/bar.hpp"
#include "talcite/skin.hpp"

#include <algorithm>

namespace talcite::ui {

namespace {

constexpr int stepping = 3;

}

// **Zawiadamiamy tylko o zmianie i tylko stąd.** Przycięcie do bieżącego okna dzieje się
// w `paint`, więc nadawanie z każdej zmiany `shift_` znaczyłoby podmienianie drzewa w trakcie
// składania klatki. Skutek jest znany i przyjęty: zwężenie okna, które samo dosunie zawartość,
// nie zawiadomi nikogo.
void Scroll::shift(Spot given) {
    const Spot want{std::max(given.x, 0), std::max(given.y, 0)};

    if (want == shift_) return;

    shift_ = want;

    ScrollEvent moved;

    (void)trigger(moved);
}

// Treść pytamy bez ograniczenia w obu kierunkach naraz — napis, który przy wąskim oknie by się
// zawinął, w przewijaku zostaje jednym długim wierszem i to jest właśnie to, czego się po nim
// spodziewamy.
Size Scroll::wants(Size room) const {
    if (children().empty()) return {};

    const int aside = style_.progress && room.wide > 0 ? 1 : 0;
    const int below = style_.progress && room.high > 0 ? 1 : 0;

    // Pytamy o `plenty` **pomniejszone o pasek**: dziecko oddające to, co dostało — panel, zasłona,
    // okno — oddałoby inaczej `plenty`, a dodanie do tego jedynki przepełnia `int` i rozmiar wychodzi
    // ujemny. Przewijak znikał wtedy z ekranu.
    const Size wants = children().front()->wants({plenty - aside, plenty - below});

    return {std::min(wants.wide + aside, room.wide), std::min(wants.high + below, room.high)};
}

View Scroll::paint(Size given, Area seen) {
    View made{seen.size};

    // Przezroczysty tam, gdzie nic nie rysuje — reguła opisana przy `Node::paint`.

    made.bare({{0, 0}, seen.size});

    if (children().empty()) return made;

    Node& one = *children().front();

    // Pionowy pasek zabiera kolumnę z prawej, poziomy — dolny wiersz. Narożnik między nimi
    // zostaje pusty, bo nie należy do żadnego z nich.
    const int aside = style_.progress && given.wide > 0 ? 1 : 0;
    const int below = style_.progress && given.high > 0 ? 1 : 0;

    const Size wants = one.wants({plenty, plenty});

    whole_ = {std::max(wants.wide, 0), std::max(wants.high, 0)};
    window_ = {given.wide - aside, given.high - below};

    shift_ = {std::clamp(shift_.x, 0, std::max(whole_.wide - window_.wide, 0)),
              std::clamp(shift_.y, 0, std::max(whole_.high - window_.high, 0))};

    // Treść jest większa od okna, a okno mniejsze o paski — dlatego przecinamy dwa razy:
    // najpierw widocznością, potem samym oknem bez pasków.
    const Area where{{-shift_.x, -shift_.y}, whole_};
    const Area part = Area::crossed(Area::crossed(seen, {{0, 0}, window_}), where);

    made.within(one.render(where, part - where.at), part.at - seen.at);

    if (aside != 0)
        edged(made, given, seen, true, window_.high, static_cast<std::size_t>(shift_.y),
              static_cast<std::size_t>(whole_.high), static_cast<std::size_t>(window_.high),
              skin(), bar_);

    if (below != 0)
        edged(made, given, seen, false, window_.wide, static_cast<std::size_t>(shift_.x),
              static_cast<std::size_t>(whole_.wide), static_cast<std::size_t>(window_.wide),
              skin(), bar_);

    return made;
}

// Kółko przewija w pionie, a z Shiftem w poziomie — tak robi większość programów i tak działa
// jedyna mysz, która ma tylko jedno kółko.
bool Scroll::react(WheelEvent& what) {
    const Spot was = shift_;

    if (what.mods().shift)
        shift_.x = std::clamp(shift_.x - what.steps() * stepping, 0,
                              std::max(whole_.wide - window_.wide, 0));
    else
        shift_.y = std::clamp(shift_.y - what.steps() * stepping, 0,
                              std::max(whole_.high - window_.high, 0));

    if (shift_ == was) return false;

    ScrollEvent moved;

    (void)trigger(moved);

    return true;
}

}
