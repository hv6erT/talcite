#include "talcite/button.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

#include "talcite/encoding.hpp"
#include "talcite/root.hpp"
#include "talcite/skin.hpp"

#include "eliding.hpp"

#include "marking.hpp"

namespace talcite::ui {

namespace {

// Klamry wokół napisu: zwykły przycisk ma ich cztery kratki, podstawowy sześć — `[ Tak ]`
// i `[< Tak >]`, dokładnie jak w Midnight Commanderze.
int around(bool primary) { return primary ? 6 : 4; }

}

Button::Button(std::string text) : text_(std::move(text)) { role("talcite.button"); }

int Button::capacity() const { return 0; }

Size Button::wants(Size room) const {
    const int wide = measured(text_).wide + around(primary_);

    return {std::min(wide, room.wide), std::min(1, room.high)};
}

View Button::paint(Size given, Area seen) {
    View made{seen.size};

    // **Przezroczysty tam, gdzie nic nie rysuje** — reguła opisana przy `Node::paint`. Przycisk
    // maluje jeden wiersz, a wiersz układu daje mu swoją wysokość, więc bez tego reszta prostokąta
    // szła do rodzica jako nieprzezroczyste spacje i zakrywała jego tło.
    made.clear({{0, 0}, seen.size});

    const Skin& how = skin();

    // Rola na stan, a nie jedna rola z doklejanymi znacznikami: autor skórki chce powiedzieć, jak
    // wygląda **przycisk z ogniskiem**, a nie zgadywać, co biblioteka do niego dorobi.
    // Rola na stan powstaje z **rdzenia i końcówki**: rdzeń zadaje ten, kto buduje przycisk,
    // a końcówkę dokłada przycisk, bo tylko on wie, jakie ma stany. Skórka opisuje więc rodzinę
    // `<rdzeń>`, `<rdzeń>.focus`, `<rdzeń>.key`, `<rdzeń>.off` i nie musi nic zgadywać.
    //
    // Sklejenie nazwy przydziela pamięć raz na stan i raz na klatkę. Przy trzech przyciskach
    // w oknie to jest nic, a trzymanie czterech gotowych napisów w każdym przycisku kosztowałoby
    // więcej i przy każdej zmianie rdzenia trzeba by je odświeżać.
    const Style look = how.style(!enabled()      ? role() + ".disabled"
                                      : focused() ? role() + ".focus"
                                                  : role());

    // **Jeden wiersz, nie tyle, ile dano.** Przycisk zgłasza w `wants()` wysokość jeden, a wiersz
    // daje dzieciom swoją własną wysokość — bez tego negatyw rozlewałby się na cały wiersz.
    made.fill({{-seen.at.x, -seen.at.y}, {given.wide, 1}}, look);

    const std::string first = primary_ ? "[< " : "[ ";
    const std::string after = primary_ ? " >]" : " ]";

    const int aside = measured(first).wide;
    const int apart = measured(after).wide;

    // Za wąski przycisk ucina **napis**, a nie klamrę zamykającą. Klamra jest tym, co czyni go
    // przyciskiem; urwana zostawiała po sobie coś, co wyglądało jak zwykły tekst.
    const int room = std::max(given.wide - aside - apart, 0);

    const std::string shown =
        elided(text_, room, Elide::right, how.glyph("talcite.elide"));

    const int inner = measured(shown).wide;

    made.put({-seen.at.x, -seen.at.y}, first, look);
    made.put({aside - seen.at.x, -seen.at.y}, shown, look);
    made.put({aside + inner - seen.at.x, -seen.at.y}, after, look);

    // Znakowanie liczymy po **przyciętym** napisie: litera, której po przycięciu nie widać, nie ma
    // czego podkreślać, a policzona po całym napisie trafiłaby w wielokropek albo w klamrę.
    // Litera aktywna ma własną rolę, nakładaną na wygląd przycisku — dzięki temu skórka może dać
    // jej barwę, a wbudowana zostaje przy podkreśleniu, które widać na każdym terminalu.
    talcite::ui::marking(made, {aside - seen.at.x, -seen.at.y}, shown, shortcut_column(shown),
                      look, how.style(role() + ".key"));

    return made;
}

// **Naciśnięcie idzie przez `trigger`**, a nie prosto do wywołania zwrotnego. Dzięki temu jedno
// zdarzenie zobaczy i sam widżet, i wszystko nad nim, a `on<PickEvent>` łapie naciśnięcie
// niezależnie od tego, czym je wywołano — Enterem, myszą czy klawiszem aktywnym. Wywołanie
// zwrotne omijało cały mechanizm i było jedynym miejscem w talcite, gdzie zdarzenia nie było widać.
void Button::pick() {
    if (!enabled()) return;

    (void)trigger(PickEvent{});
}

// Enter i spacja naciskają ten przycisk, który ma ognisko — tak jest w Midnight Commanderze
// i nie zależy to od klawiszy aktywnych. Przyciskiem podstawowym i klawiszami aktywnymi
// z dowolnego miejsca zajmuje się dialog, bo to są zachowania okna, a nie przycisku.
//
// Ze sterującym nie łapiemy: Ctrl+Enter i Alt+spacja to z założenia inne skróty. Tak samo odsiewa
// je legenda.
bool Button::handle(KeyEvent& what) {
    if (!enabled()) return false;

    if (what.mods().ctrl || what.mods().alt) return false;

    // **Sam Enter, bez spacji.** Spacja jest odruchem z innego świata niż Midnight Commander,
    // a każdy klawisz zabrany przez widżet to jeden, którego aplikacja nie może użyć — w oknie
    // z polem tekstowym spacja i tak należy do pisania.
    if (what.key().code() != Code::enter) return false;

    pick();

    return true;
}

// Klawisz aktywny przychodzi z bazy — ona sprawdziła, że jest nasz. Zostaje powiedzieć, co robimy,
// a przycisk robi to samo, co przy Enterze i przy kliknięciu.
void Button::activate() { pick(); }

// Klik w przycisk **bierze ognisko** — inaczej niż pole legendy, które go nie rusza. Tam liczyło
// się to, co zaznaczone w panelu; tutaj kliknięty przycisk staje się tym, na którym stoisz.
bool Button::handle(ClickEvent&) {
    if (!enabled()) return true;

    if (root() != nullptr) focus();

    pick();

    return true;
}

}
