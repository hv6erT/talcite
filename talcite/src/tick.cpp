#include "talcite/tick.hpp"

#include <algorithm>
#include <utility>

#include "talcite/encoding.hpp"
#include "talcite/root.hpp"
#include "talcite/skin.hpp"

#include "eliding.hpp"

#include "marking.hpp"

namespace talcite::ui {

namespace {

// Klamra ze znakiem i spacja za nią: `[x] `. Cztery kratki, zawsze.
constexpr int around = 4;

}

Tick::Tick(std::string text, bool on) : text_(std::move(text)), on_(on) { role("talcite.tick"); }

// **Ustawienie z programu nie zawiadamia.** Wypełnienie okna wartościami początkowymi uruchamiałoby
// inaczej wszystko, co do tych wartości podpięto — a to jest wypełnianie, nie naciskanie.
void Tick::ticked(bool given) { on_ = given; }

// Odwrócenie stanu **razem z zawiadomieniem**. Tędy idzie spacja, kliknięcie i klawisz aktywny.
void Tick::pick() {
    if (!enabled()) return;

    on_ = !on_;

    (void)trigger(PickEvent{});
}

int Tick::capacity() const { return 0; }

// Jeden wiersz, nie tyle, ile dano — tak samo jak przycisk. Wiersz układu daje dzieciom swoją
// wysokość, a bez tego negatyw ogniska rozlałby się na całą jego wysokość.
Size Tick::wants(Size room) const {
    return {std::min(measured(text_).wide + around, room.wide), std::min(1, room.high)};
}

View Tick::paint(Size given, Area seen) {
    View made{seen.size};

    // Przezroczysty tam, gdzie nic nie rysuje — ta sama reguła i ten sam powód co w przycisku.
    made.clear({{0, 0}, seen.size});

    const Skin& how = skin();

    // Rola na stan, złożona z rdzenia i końcówki — ta sama umowa co w przycisku, żeby autor skórki
    // opisywał rodzinę `<rdzeń>`, `<rdzeń>.focus`, `<rdzeń>.off` i niczego nie zgadywał.
    const Style look = how.style(!enabled() ? role() + ".disabled" : focused() ? role() + ".focus" : role());

    made.fill({{-seen.at.x, -seen.at.y}, {given.wide, 1}}, look);

    const std::string first = on_ ? "[x] " : "[ ] ";

    // Za wąskie pole ucina **napis**, a nie klamrę. Klamra jest tym, co czyni z tego pole wyboru;
    // urwana zostawiałaby po sobie coś, co wygląda jak zwykły tekst.
    const int room = std::max(given.wide - around, 0);

    made.put({-seen.at.x, -seen.at.y}, first, look);

    if (room <= 0) return made;

    const std::string shown =
        elided(text_, room, Elide::right, how.glyph("talcite.elide"));

    made.put({around - seen.at.x, -seen.at.y}, shown, look);

    // Znakowanie liczymy po **przyciętym** napisie: litera, której po przycięciu nie widać, nie ma
    // czego podkreślać. Klawisz aktywny bez podkreślenia byłby zresztą klawiszem, o którym nikt się
    // nie dowie — a w Midnight Commanderze pole wyboru swoją literę pokazuje.
    talcite::ui::marking(made, {around - seen.at.x, -seen.at.y}, shown, shortcut_column(shown),
                      look, how.style(role() + ".key"));

    return made;
}

// **Spacja odwraca, Enter idzie wyżej.** W oknie dialogowym Enter należy do przycisku podstawowego,
// więc pole wyboru, które by go połknęło, psułoby zatwierdzanie okna.
bool Tick::handle(KeyEvent& what) {
    if (!enabled()) return false;

    if (what.mods().ctrl || what.mods().alt) return false;

    if (what.key().letter() != U' ') return false;

    pick();

    return true;
}

bool Tick::handle(ClickEvent&) {
    if (!enabled()) return true;

    if (root() != nullptr) focus();

    pick();

    return true;
}

// Klawisz aktywny przychodzi z bazy — ona sprawdziła, że jest nasz. Ptaszek robi to samo, co przy
// spacji i przy kliknięciu: odwraca się i zawiadamia.
void Tick::activate() { pick(); }

}
