#include "talcite/scroll.hpp"

#include <algorithm>
#include <utility>

#include "talcite/skin.hpp"

#include "followed.hpp"
#include "scrollbar.hpp"

namespace talcite::ui {

namespace {

// Trzy wiersze na ząbek kółka — tyle daje większość terminali i tyle wyrabia oko.
constexpr int stepping = 3;

// **Paski: zawsze albo tylko wtedy, gdy trzeba** (`needed`). Pasek „gdy trzeba" stoi na osi, na
// której treść nie mieści się w oknie; treść pytana jest bez pasków, a gdy jeden stanie, drugi oś
// sprawdza jeszcze raz — pasek pionowy zabiera kolumnę, więc zawinięta treść może urosnąć i dopiero
// wtedy nie zmieścić się w poziomie (i odwrotnie). Dwa przejścia wystarczą: pasków są dwa i żaden
// nie znika, gdy już stanął.
std::pair<int, int> bars(const ScrollShape& shape, bool across, bool down, const Node& child, Size room) {
    if (!shape.scrollbar) return {0, 0};

    int aside = down && room.wide > 0 ? 1 : 0;
    int below = across && room.high > 0 ? 1 : 0;

    if (!shape.needed) return {aside, below};

    const bool vertical = aside != 0;
    const bool horizontal = below != 0;

    aside = 0;
    below = 0;

    for (int pass = 0; pass < 2; ++pass) {
        const Size inner{room.wide - aside, room.high - below};
        const Size wants = child.wants({across ? plenty : inner.wide, down ? plenty : inner.high});

        if (vertical && wants.high > inner.high) aside = 1;
        if (horizontal && wants.wide > inner.wide) below = 1;
    }

    return {aside, below};
}

}

// **Jeden przewijak, trzy osie.** Dawniej były trzy klasy — VScroll, HScroll i Scroll — z nagłówkami
// identycznymi co do litery i ciałami, które różniły się tylko tym, który wymiar dziecko dostaje
// dopasowany, a który wolny. Oś otwarta (`across` w poziomie, `down` w pionie) pyta dziecko
// o `plenty`, zamknięta — o tyle, ile jest; na zamkniętej osi treść jest tak duża jak okno, więc
// przewinięcie w niej zawsze wychodzi zero i nic osobnego nie trzeba pilnować.

Scroll::Scroll(Axis axis, ScrollShape shape)
    : axis_(axis), shape_(shape), upright_(std::make_shared<Grip>()), sideways_(std::make_shared<Grip>()) {}

// **Zawiadamiamy tylko o zmianie i tylko stąd.** Przycięcie do bieżącego okna dzieje się
// w `paint`, więc nadawanie z każdej zmiany `shift_` znaczyłoby podmienianie drzewa w trakcie
// składania klatki. Skutek jest znany i przyjęty: zwężenie okna, które samo dosunie zawartość,
// nie zawiadomi nikogo. Składowa zamkniętej osi jest pomijana.
void Scroll::shift(Spot given) {
    const Spot want{across() ? std::max(given.x, 0) : 0, down() ? std::max(given.y, 0) : 0};

    if (want == shift_) return;

    shift_ = want;

    (void)trigger(ScrollEvent{});
}

// Pytamy o `plenty` **pomniejszone o pasek**: dziecko oddające to, co dostało — panel, zasłona,
// okno — oddałoby inaczej `plenty`, a dodanie do tego jedynki przepełnia `int` i rozmiar wychodzi
// ujemny. Przewijak znikał wtedy z ekranu. Oddajemy co najwyżej tyle, ile sami dostaliśmy: rodzic
// nie ma skąd wziąć więcej. Napis, który przy wąskim oknie by się zawinął, w przewijaku otwartym
// w poziomie zostaje jednym długim wierszem — i to jest właśnie to, czego się po nim spodziewamy.
Size Scroll::wants(Size room) const {
    if (children().empty()) return {};

    const auto [aside, below] = bars(shape_, across(), down(), *children().front(), room);

    const Size wants = children().front()->wants(
        {across() ? plenty - aside : room.wide - aside, down() ? plenty - below : room.high - below});

    return {std::min(wants.wide + aside, room.wide), std::min(wants.high + below, room.high)};
}



// Dziecko stoi u nas w prostokącie przesuniętym o przewinięcie i tak dużym, jak samo chciało na
// osiach otwartych. Widoczna jest z tego tylko część wspólna z oknem bez pasków — i tylko tę część
// dziecko w ogóle narysuje, bo dostaje ją jako widoczność.
View Scroll::paint(Size given, Area seen) {
    View made{seen.size};

    // Przezroczysty tam, gdzie nic nie rysuje — reguła opisana przy `Node::paint`.

    made.clear({{0, 0}, seen.size});

    if (children().empty()) return made;

    Node& one = *children().front();

    // Pionowy pasek zabiera kolumnę z prawej, poziomy — dolny wiersz. Narożnik między nimi
    // zostaje pusty, bo nie należy do żadnego z nich.
    const auto [aside, below] = bars(shape_, across(), down(), *children().front(), given);

    window_ = {given.wide - aside, given.high - below};

    const Size wants = one.wants({across() ? plenty : window_.wide, down() ? plenty : window_.high});

    whole_ = {across() ? std::max(wants.wide, 0) : window_.wide,
              down() ? std::max(wants.high, 0) : window_.high};

    // Treść mogła się skrócić między klatkami — wtedy przewinięcie wisiałoby za jej końcem.
    shift_ = {std::clamp(shift_.x, 0, std::max(whole_.wide - window_.wide, 0)),
              std::clamp(shift_.y, 0, std::max(whole_.high - window_.high, 0))};

    // Treść jest większa od okna, a okno mniejsze o paski — dlatego przecinamy dwa razy:
    // najpierw widocznością, potem samym oknem bez pasków.
    const Area where{{-shift_.x, -shift_.y}, whole_};
    const Area part = Area::crossed(Area::crossed(seen, {{0, 0}, window_}), where);

    made.draw(one.render(where, part - where.at), part.at - seen.at);

    // **Za ogniskiem — w `paint`, jak w liście**, bo dopiero po rozstawieniu dzieci znany jest
    // prostokąt węzła, który ognisko wziął. Gdy leży poza oknem, dosuwamy przewinięcie i rysujemy
    // treść drugi raz — raz na zmianę ogniska, nie na klatkę. Kółko przesuwa widok i ogniska nie
    // rusza, więc oba nie kłócą się o `shift_`. Bez zawiadomienia, jak przy przycięciu: klatka
    // w toku nie jest porą na podmienianie drzewa. Na osi zamkniętej `followed` przycina do zera.
    if (follow_) {
        follow_ = false;

        if (const Node* held = beheld(*this); held != nullptr)
            for (int pass = 0; pass < passes; ++pass) {
                const std::optional<Area> box = held->area_in(one);

                if (!box.has_value()) break;

                const Spot want{followed(shift_.x, box->at.x, box->size.wide, window_.wide, whole_.wide),
                                followed(shift_.y, box->at.y, box->size.high, window_.high, whole_.high)};

                if (want == shift_) break;

                shift_ = want;

                const Area again{{-shift_.x, -shift_.y}, whole_};
                const Area piece = Area::crossed(Area::crossed(seen, {{0, 0}, window_}), again);

                made.clear({{0, 0}, seen.size});
                made.draw(one.render(again, piece - again.at), piece.at - seen.at);
            }
    }

    upright_->laid(std::nullopt);
    sideways_->laid(std::nullopt);

    if (aside != 0) {
        const Scrollbar bar{.at = Scrollbar::edge(given, seen, true),
                            .much = window_.high,
                            .shift = static_cast<std::size_t>(shift_.y),
                            .whole = static_cast<std::size_t>(whole_.high),
                            .window = static_cast<std::size_t>(window_.high),
                            .skin = skin(),
                            .role = scrollbar_role_};

        bar.draw(made);

        Track mine = bar.track();

        mine.at = Scrollbar::edge(given, {{0, 0}, given}, true);

        upright_->laid(mine);
    }

    if (below != 0) {
        const Scrollbar bar{.at = Scrollbar::edge(given, seen, false),
                            .upright = false,
                            .much = window_.wide,
                            .shift = static_cast<std::size_t>(shift_.x),
                            .whole = static_cast<std::size_t>(whole_.wide),
                            .window = static_cast<std::size_t>(window_.wide),
                            .skin = skin(),
                            .role = scrollbar_role_};

        bar.draw(made);

        Track mine = bar.track();

        mine.at = Scrollbar::edge(given, {{0, 0}, given}, false);

        sideways_->laid(mine);
    }

    return made;
}

// Ognisko wzięło któreś z dzieci — zdarzenie bąbelkuje w górę — więc następna klatka ma za nim
// pójść. Nie trzymamy wskaźnika: węzeł może zginąć przed klatką, a `Root` i tak wie, kto ma
// ognisko. Zdarzenia nie bierzemy, żeby rodzice też je zobaczyli — przewijak w przewijaku.
bool Scroll::handle(FocusEvent&) {
    follow_ = true;

    return false;
}

// Kółko przewija w pionie, a z Shiftem w poziomie — tak robi większość programów i tak działa
// jedyna mysz, która ma tylko jedno kółko. Przewijak tylko poziomy przewija kółkiem w poziomie, bez
// Shifta. Na końcu przewijania oddajemy false, więc zdarzenie idzie w górę — przewijak
// w przewijaku przekazuje ruch dalej, zamiast go połykać.
bool Scroll::handle(WheelEvent& what) {
    const Spot was = shift_;

    const bool sideways = axis_ == Axis::horizontal || (axis_ == Axis::both && what.mods().shift);

    if (sideways)
        shift_.x = std::clamp(shift_.x - what.steps() * stepping, 0,
                              std::max(whole_.wide - window_.wide, 0));
    else
        shift_.y = std::clamp(shift_.y - what.steps() * stepping, 0,
                              std::max(whole_.high - window_.high, 0));

    if (shift_ == was) return false;

    (void)trigger(ScrollEvent{});

    return true;
}

// **Mysz na pasku** (U6 w twa), na każdej osi swoim chwytem: strzałka — kratka, tor — okno,
// suwak — ciągnięcie. Ruch przy chwycie idzie do paska, który chwycono, gdziekolwiek jest kursor.
bool Scroll::handle(MouseEvent& what) {
    const Spot here = local(what.where());

    const bool sideways = sideways_->held() || (!upright_->held() && sideways_->over(here));

    Grip& grip = sideways ? *sideways_ : *upright_;

    const Asked asked = grip.mouse(what, here, *this);

    const int window = sideways ? window_.wide : window_.high;
    const int now = sideways ? shift_.x : shift_.y;

    const auto moved = [this, sideways](int to) {
        const int most = sideways ? std::max(whole_.wide - window_.wide, 0) : std::max(whole_.high - window_.high, 0);
        const int fitted = std::clamp(to, 0, most);

        shift(sideways ? Spot{fitted, shift_.y} : Spot{shift_.x, fitted});
    };

    switch (asked.part) {
        case Part::none: return false;
        case Part::back: moved(now - 1); break;
        case Part::forth: moved(now + 1); break;
        case Part::before: moved(now - std::max(window, 1)); break;
        case Part::after: moved(now + std::max(window, 1)); break;
        case Part::thumb:
            if (asked.to.has_value()) moved(static_cast<int>(*asked.to));
            break;
    }

    return true;
}

}
