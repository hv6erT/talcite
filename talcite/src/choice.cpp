#include "talcite/choice.hpp"

#include <algorithm>
#include <utility>

#include "talcite/encoding.hpp"
#include "talcite/root.hpp"
#include "talcite/skin.hpp"

#include "eliding.hpp"


namespace talcite::ui {

namespace {

// Klamra ze znakiem i spacja za nią: `(•) `. Cztery kratki, tak jak w polu wyboru.
constexpr int around = 4;

}

Choice::Choice() : OptionsNode("talcite.choice") {}

Choice::Choice(std::vector<std::string> options, std::size_t which) : Choice() {
    this->options().all(std::move(options));

    this->which(which);
}

// **Grupa bierze ognisko na siebie**, a nie poszczególne opcje: Tab wchodzi do niej raz i raz z niej
// wychodzi, a strzałki chodzą w środku. Ten sam kształt ma lista i tak samo zachowuje się grupa
// w Midnight Commanderze.
bool Choice::takes_focus() const { return !options().empty(); }

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

    made.clear({{0, 0}, seen.size});

    const Skin& how = skin();

    // Znak wyboru bierze się ze **skórki**, a nie z kodu: terminal bez Unicode dostanie gwiazdkę,
    // tak samo jak wielokropek przycinania bierze się z `talcite.elide`.
    const char32_t dot = how.glyph(role() + ".dot");

    const Style plain = how.style(enabled() ? role() : role() + ".disabled");
    const Style focus = how.style(enabled() ? role() + ".focus" : role() + ".disabled");

    const int room = std::max(given.wide - around, 0);

    for (std::size_t at = 0; at < options().many(); ++at) {
        const int y = static_cast<int>(at) - seen.at.y;

        if (static_cast<int>(at) >= given.high) break;

        // Podświetlona jest **wybrana opcja, i tylko gdy grupa ma ognisko** — inaczej okno bez
        // ogniska w grupie wyglądałoby, jakby stało na niej.
        const bool here = at == this->which() && focused();

        const Style look = here ? focus : plain;

        made.fill({{-seen.at.x, y}, {given.wide, 1}}, look);

        std::string first = "(";

        if (at == this->which())
            utf8::encode(dot, first);
        else
            first += ' ';

        first += ") ";

        made.put({-seen.at.x, y}, first, look);

        if (room > 0)
            made.put({around - seen.at.x, y},
                        elided(options()[at], room, Elide::right,
                               how.glyph("talcite.elide")),
                        look);
    }

    return made;
}

// **Ruch jest wyborem**: strzałka od razu przestawia zaznaczenie, tak jak w Midnight Commanderze,
// a nie przesuwa jakiegoś osobnego kursora do zatwierdzenia spacją. Spacja i tak działa, bo bywa
// odruchem. Enter, Tab i litery idą wyżej — Enter należy do przycisku podstawowego okna.
bool Choice::handle(KeyEvent& what) {
    if (!enabled() || options().empty()) return false;

    if (what.mods().ctrl || what.mods().alt) return false;

    const std::size_t was = *which();

    switch (what.key().code()) {
        case Code::up:
            if (was == 0) return false;

            which(was - 1);

            break;

        case Code::down:
            if (was + 1 >= options().many()) return false;

            which(was + 1);

            break;

        case Code::home: which(0); break;

        case Code::end: which(options().many() - 1); break;

        default:
            if (what.key().letter() != U' ') return false;

            break;
    }

    if (*which() == was) return true;

    (void)trigger(PickEvent{});

    return true;
}

bool Choice::handle(ClickEvent& what) {
    if (!enabled() || options().empty()) return true;

    if (root() != nullptr) focus();

    const int y = local(what.where()).y;

    if (y < 0 || static_cast<std::size_t>(y) >= options().many()) return true;

    const std::size_t was = *which();

    which(static_cast<std::size_t>(y));

    if (*which() == was) return true;

    (void)trigger(PickEvent{});

    return true;
}

}
