#include "talcite/choice.hpp"

#include <algorithm>
#include <utility>

#include "talcite/encoding.hpp"
#include "talcite/root.hpp"
#include "talcite/skin.hpp"

#include "eliding.hpp"

#include "placing.hpp"

namespace talcite::ui {

namespace {

// Klamra ze znakiem i spacja za nią: `(•) `. Cztery kratki, tak jak w polu wyboru.
constexpr int around = 4;

}

Choice::Choice() : OptionsNode("talcite.choice") {}

Choice::Choice(std::vector<std::string> options, std::size_t which) : Choice() {
    this->options().all(std::move(options));

    at(which);
}

// **Grupa bierze ognisko na siebie**, a nie poszczególne opcje: Tab wchodzi do niej raz i raz z niej
// wychodzi, a strzałki chodzą w środku. Ten sam kształt ma lista i tak samo zachowuje się grupa
// w Midnight Commanderze.
bool Choice::focusable() const { return able() && !options().empty(); }

Size Choice::wants(Size room) const {
    int wide = 0;

    for (const std::string& one : options()) wide = std::max(wide, measured(one).wide);

    return {std::min(wide + around, room.wide),
            std::min(static_cast<int>(options().many()), room.high)};
}

View Choice::paint(Size given, Area seen) {
    // **Ognisko oddajemy leniwie, przy rysowaniu**, a nie w chwili opróżnienia zbioru: `Options`
    // nie ma wskazania na właściciela, więc nie ma jak go zawiadomić. Grupa bez opcji nie jest już
    // ogniskowalna, a ognisko na węźle, który go nie przyjmuje, nie ma prawa zostać.
    if (focused() && !focusable()) unfocus();

    View made{seen.size};

    made.bare({{0, 0}, seen.size});

    const Skin& how = skin();

    // Znak wyboru bierze się ze **skórki**, a nie z kodu: terminal bez Unicode dostanie gwiazdkę,
    // tak samo jak wielokropek przycinania bierze się z `talcite.elide`.
    const char32_t dot = how.get<char32_t>(role() + ".dot");

    const Style plain = how.get<Style>(able() ? role() : role() + ".off");
    const Style focus = how.get<Style>(able() ? role() + ".focus" : role() + ".off");

    const int room = std::max(given.wide - around, 0);

    for (std::size_t at = 0; at < options().many(); ++at) {
        const int y = static_cast<int>(at) - seen.at.y;

        if (static_cast<int>(at) >= given.high) break;

        // Podświetlona jest **wybrana opcja, i tylko gdy grupa ma ognisko** — inaczej okno bez
        // ogniska w grupie wyglądałoby, jakby stało na niej.
        const bool here = at == this->at() && focused();

        const Style look = here ? focus : plain;

        made.fill({{-seen.at.x, y}, {given.wide, 1}}, look);

        std::string first = "(";

        if (at == this->at())
            spelled(dot, first);
        else
            first += ' ';

        first += ") ";

        made.string({-seen.at.x, y}, first, look);

        if (room > 0)
            made.string({around - seen.at.x, y},
                        elided(options()[at], room, ElideStyle::right,
                               how.get<char32_t>("talcite.elide")),
                        look);
    }

    return made;
}

// **Ruch jest wyborem**: strzałka od razu przestawia zaznaczenie, tak jak w Midnight Commanderze,
// a nie przesuwa jakiegoś osobnego kursora do zatwierdzenia spacją. Spacja i tak działa, bo bywa
// odruchem. Enter, Tab i litery idą wyżej — Enter należy do przycisku podstawowego okna.
bool Choice::react(KeyEvent& what) {
    if (!able() || options().empty()) return false;

    if (what.mods().ctrl || what.mods().alt) return false;

    const std::size_t was = at();

    switch (what.key().code()) {
        case Code::up:
            if (at() == 0) return false;

            at(at() - 1);

            break;

        case Code::down:
            if (at() + 1 >= options().many()) return false;

            at(at() + 1);

            break;

        case Code::home: at(0); break;

        case Code::end: at(options().many() - 1); break;

        default:
            if (what.key().letter() != U' ') return false;

            break;
    }

    if (at() == was) return true;

    PickEvent one;

    (void)trigger(one);

    return true;
}

bool Choice::react(ClickEvent& what) {
    if (!able() || options().empty()) return true;

    if (root() != nullptr) focus();

    const int y = own(*this, what.where()).y;

    if (y < 0 || static_cast<std::size_t>(y) >= options().many()) return true;

    const std::size_t was = at();

    at(static_cast<std::size_t>(y));

    if (at() == was) return true;

    PickEvent one;

    (void)trigger(one);

    return true;
}

}
