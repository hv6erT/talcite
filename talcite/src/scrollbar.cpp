#include "scrollbar.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>

#include "talcite/node.hpp"
#include "talcite/root.hpp"
#include "talcite/skin.hpp"

namespace talcite::ui {

// Pasek jest jedną kratką szeroki: strzałka na początku, strzałka na końcu, a między nimi tor
// z suwakiem. Suwak mówi dwie rzeczy naraz — gdzie jesteśmy **i** jaka część całości mieści się
// w oknie — bo jego długość jest ułamkiem toru w tej samej proporcji.
namespace {

// **Pasek liczy proporcję, nie sztuki.** Wolno mu więc podzielić wszystkie trzy liczby wspólnym
// dzielnikiem, byle całość zmieściła się w `int` — a to jest jedyny sposób, żeby dokument
// o dwóch miliardach wierszy albo bajtów w ogóle dał się narysować.
struct Scaled {
    int shift = 0;
    int whole = 0;
    int window = 0;
};

Scaled scaled(std::size_t shift, std::size_t whole, std::size_t window) {
    constexpr auto brim = static_cast<std::size_t>(std::numeric_limits<int>::max());

    std::size_t by = 1;

    while (whole / by > brim) by *= 2;

    return {static_cast<int>(shift / by), static_cast<int>(whole / by),
            static_cast<int>(window / by)};
}

// Dzielnik, którym `scaled` zmieściło liczby w `int` — potrzebny w drugą stronę, przy ciągnięciu.
std::size_t divisor(std::size_t whole) {
    constexpr auto brim = static_cast<std::size_t>(std::numeric_limits<int>::max());

    std::size_t by = 1;

    while (whole / by > brim) by *= 2;

    return by;
}

// **Rozkład paska liczony w jednym miejscu** — rysowanie i mysz muszą widzieć suwak w tych samych
// kratkach, inaczej klik w narysowany suwak trafiałby w tor obok.
struct Laid {
    int track = 0;
    int thumb = 0;
    int from = 0;
    int most = 0;
};

Laid laid_out(const Track& one) {
    Laid made;

    made.track = one.much - 2;

    if (made.track <= 0) return made;

    const Scaled small = scaled(one.shift, one.whole, one.window);

    made.most = std::max(small.whole - small.window, 0);

    // Gdy treść mieści się w oknie, suwak zajmuje cały tor. Inaczej trzeba by rysować pasek,
    // który niczego nie pokazuje, a mimo to zajmuje miejsce.
    //
    // **Iloczyny liczymy w 64 bitach.** Skalowanie mieści w `int` każdą z liczb z osobna, ale nie
    // ich iloczyn z długością toru: plik trzech miliardów bajtów przewinięty do połowy dawał
    // `room * shift` rzędu miliardów razy kilka i suwak lądował byle gdzie.
    const auto widened = [](int value) { return static_cast<std::int64_t>(value); };

    made.thumb = made.most == 0 ? made.track
                                : static_cast<int>(std::clamp<std::int64_t>(
                                      widened(made.track) * small.window / std::max(small.whole, 1), 1, made.track));

    const int room = made.track - made.thumb;

    made.from = made.most == 0 ? 0 : static_cast<int>((widened(room) * small.shift + made.most / 2) / made.most);

    return made;
}

}

Spot Scrollbar::edge(Size given, Area seen, bool upright) {
    return upright ? Spot{given.wide - 1 - seen.at.x, -seen.at.y}
                   : Spot{-seen.at.x, given.high - 1 - seen.at.y};
}

void Scrollbar::draw(View& into) const {
    if (much <= 0) return;

    // Pasek pyta skórkę sam, bo nie jest węzłem i nie ma przodka, przez którego mógłby do niej
    // dojść. Wołający podaje ją z `Node::skin()` — jedno wskazanie zamiast dwóch stylów w polach.
    const Style look = skin.style(role);
    const Style full = skin.style(role + ".thumb");

    const char32_t empty = skin.glyph(role + ".track");
    const char32_t block = skin.glyph(role + ".thumb");

    into.put(at, skin.glyph(role + (upright ? ".up" : ".left")), look);

    if (much == 1) return;

    const Spot last = upright ? Spot{at.x, at.y + much - 1} : Spot{at.x + much - 1, at.y};

    into.put(last, skin.glyph(role + (upright ? ".down" : ".right")), look);

    const Laid laid = laid_out(track());

    for (int which = 0; which < laid.track; ++which) {
        const bool kept = which >= laid.from && which < laid.from + laid.thumb;
        const Spot spot =
            upright ? Spot{at.x, at.y + 1 + which} : Spot{at.x + 1 + which, at.y};

        into.put(spot, kept ? block : empty, kept ? full : look);
    }
}

// Pasek z ostatniej klatki — albo jego brak, gdy treść mieści się bez niego. Chwytu brak paska
// nie puszcza: ruch bez paska i tak nic nie ciągnie, a puszczenie przycisku chwyt zdejmuje.
void Grip::laid(std::optional<Track> given) { track_ = given; }

Part Grip::part(Spot here) const {
    if (!track_.has_value() || track_->much <= 0) return Part::none;

    const Track& one = *track_;

    const int across = one.upright ? here.x - one.at.x : here.y - one.at.y;
    const int along = one.upright ? here.y - one.at.y : here.x - one.at.x;

    if (across != 0 || along < 0 || along >= one.much) return Part::none;

    if (along == 0) return Part::back;

    if (along == one.much - 1) return Part::forth;

    const Laid laid = laid_out(one);

    const int cell = along - 1;

    if (cell < laid.from) return Part::before;

    if (cell >= laid.from + laid.thumb) return Part::after;

    return Part::thumb;
}

// Wciśnięcie lewym na pasku mówi, w co trafiło; na suwaku zapamiętuje, w której jego kratce go
// chwycono, i prosi korzeń o przytrzymanie — ruch ma dochodzić, choćby kursor zjechał z paska, jak
// przy ciągnięciu okna. Ruch przy chwycie daje przewinięcie, przy którym suwak stoi pod kursorem
// tą samą kratką; tor liczy się w tych samych proporcjach co przy rysowaniu, a wynik wraca do
// jednostek paska (wiersze, bajty) przez dzielnik skalowania. Puszczenie kończy chwyt.
Asked Grip::mouse(const MouseEvent& what, Spot here, Node& owner) {
    if (what.press() == Press::up) {
        const bool was = grip_.has_value();

        grip_.reset();

        return Asked{was ? Part::thumb : Part::none, std::nullopt};
    }

    if (what.press() == Press::move) {
        if (!grip_.has_value() || !track_.has_value()) return {};

        const Track& one = *track_;
        const Laid laid = laid_out(one);
        const int room = laid.track - laid.thumb;

        if (room <= 0 || laid.most == 0) return Asked{Part::thumb, std::nullopt};

        const int along = one.upright ? here.y - one.at.y : here.x - one.at.x;
        const int cell = std::clamp(along - 1 - *grip_, 0, room);

        const auto scaled_to = static_cast<std::size_t>(
            (static_cast<std::int64_t>(cell) * laid.most + room / 2) / room);

        // Kratka nie wychodzi poza tor, więc wynik nie wychodzi poza ostatnie przewinięcie — także
        // po pomnożeniu przez dzielnik skalowania, którym je wcześniej podzielono.
        return Asked{Part::thumb, scaled_to * divisor(one.whole)};
    }

    grip_.reset();

    if (what.button() != Mouse::left) return {};

    const Part hit = part(here);

    if (hit == Part::thumb && track_.has_value()) {
        const int along = track_->upright ? here.y - track_->at.y : here.x - track_->at.x;

        grip_ = along - 1 - laid_out(*track_).from;

        if (Root* top = owner.root(); top != nullptr) top->capture(owner);
    }

    return Asked{hit, std::nullopt};
}

}
