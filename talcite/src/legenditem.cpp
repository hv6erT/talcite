#include "talcite/legenditem.hpp"

#include <string>
#include <utility>

#include "talcite/encoding.hpp"
#include "talcite/skin.hpp"

#include "eliding.hpp"

#include "marking.hpp"

namespace talcite::ui {

LegendItem::LegendItem(Key key, std::string text) : text_(std::move(text)) {
    role("talcite.legend");

    keys().add(key);
}

// **Pole legendy nie bierze ogniska**, także włączone: odpowiada na klawisz aktywny i na kliknięcie,
// a Tab chodzi po treści okna, nie po legendzie.
bool LegendItem::takes_focus() const { return false; }

// Pole nie ma czego trzymać w środku.
int LegendItem::capacity() const { return 0; }

// Szerokość dostaje od paska, wysokość ma zawsze jedną.
Size LegendItem::wants(Size room) const { return {room.wide, 1}; }

// **Rysowana etykietka klawisza wyprowadza się z klawisza, a nie stoi obok niego w polu.** Dwa
// źródła tej samej prawdy rozjeżdżają się przy pierwszej zmianie skrótu, a tak nie da się napisać
// legendy, która kłamie o tym, co nacisnąć.
std::string LegendItem::tag() const {
    // Pole zgłasza się jednym klawiszem — pierwszym ze zbioru. Zbiór trzyma ich więcej, bo tak ma
    // każdy węzeł z klawiszami, ale etykietka jest jedna i nie ma jak pokazać dwóch naraz.
    if (keys().empty()) return {};

    const Key which = keys().front();

    const Code code = which.code();

    if (code >= Code::f1 && code <= Code::f12)
        return std::to_string(static_cast<int>(code) - static_cast<int>(Code::f1) + 1);

    if (const char32_t letter = which.letter(); letter != 0) return utf8::encode(letter);

    // Escape, Enter i reszta nazwanych klawiszy nie mają w Midnight Commanderze etykietki, więc
    // i tu jej nie wymyślamy. Pole bez etykietki oddaje samą nazwę czynności.
    return {};
}

void LegendItem::pick() {
    if (!enabled()) return;

    (void)trigger(PickEvent{});
}

// Czynność pola to jego wybranie — i to jedno zdanie jest całym powodem, dla którego pole dziedziczy
// po węźle z klawiszami. Dopasowanie klawisza, zwijanie wielkości liter i odsianie wyłączonych pól
// robi już `KeyedNode`; tutaj zostaje wyłącznie to, co dla legendy własne.
void LegendItem::activate() { pick(); }

// Wygląd Midnight Commandera: **cyfra zwykłym wyglądem, nazwa w negatywie**. Dzięki temu cyfra
// czyta się jako „to naciśnij", a nazwa jako klawisz do kliknięcia. Negatyw obejmuje dopełnienie
// do końca pola, więc sąsiednie pola stykają się bokami i pasek nie ma dziur.
//
// **Etykietka i litera aktywna to dwie drogi do tego samego**, więc nigdy nie rysujemy obu naraz.
// Klawisz funkcyjny nie ma litery, którą dałoby się podkreślić — dostaje cyfrę z boku, jak w MC.
// Klawisz-litera obecna w nazwie **zastępuje** etykietkę podkreśleniem, tak jak w pozycji menu:
// dwa razy to samo w polu szerokim na sześć kratek byłoby marnotrawstwem i szumem. Litera, której
// w nazwie nie ma, wraca do etykietki — inaczej byłaby skrótem, o którym nikt się nie dowie.
View LegendItem::paint(Size given, Area seen) {
    View made{seen.size};

    // Cyfra, litera aktywna i nazwa mają **osobne role**, więc skórka rozstrzyga o każdej z osobna.
    // Wbudowana daje cyfrze wygląd zwykły, literze podkreślenie, a nazwie negatyw — czyli to, co
    // robi MC na gołym terminalu.
    const Skin& how = skin();

    const Style gone = enabled() ? Style{} : how.style(role() + ".disabled");

    const Style plain = Style::merge(how.style(role() + ".tag"), gone);
    const Style label = Style::merge(how.style(role() + ".text"), gone);

    made.fill({{-seen.at.x, -seen.at.y}, given}, plain);

    const char32_t dots = how.glyph("talcite.elide");

    // Liczymy najpierw po napisie przyciętym do **całej** szerokości, bo taki będzie, gdy
    // etykietka odpadnie. Kolejność jest wymuszona: dopiero wynik rozstrzyga, ile miejsca zostaje
    // na nazwę — a szerokość nazwy rozstrzyga, gdzie naprawdę stoi litera.
    const std::string whole = elided(text_, given.wide, Elide::right, dots);

    // Etykietka odpada **tylko wtedy, gdy sama jest tą literą** — inaczej pokazywalibyśmy jedno
    // i to samo dwa razy w polu szerokim na sześć kratek. Pole zgłoszone klawiszem funkcyjnym
    // zatrzymuje cyfrę nawet wtedy, gdy ma w zapasie także literę: pasek czyta się jako ciąg
    // numerów i dziura w tym ciągu byłaby gorsza niż brak podkreślenia.
    const bool named = !keys().empty() && keys().front().letter() != 0;

    const std::string what = named && shortcut_column(whole) >= 0 ? std::string{} : tag();

    const int aside = measured(what).wide;

    if (aside > 0) made.put({-seen.at.x, -seen.at.y}, cut(what, given.wide), plain);

    const int room = given.wide - aside;

    if (room <= 0) return made;

    made.fill({{aside - seen.at.x, -seen.at.y}, {room, 1}}, label);

    if (text_.empty()) return made;

    const std::string shown = aside == 0 ? whole : elided(text_, room, Elide::right, dots);

    made.put({aside - seen.at.x, -seen.at.y}, shown, label);

    // Kolumnę liczymy po **tym** napisie, a nie po tamtym: z etykietką nazwa jest węższa, więc
    // litera, którą widać w szerszej, w tej może już siedzieć w wielokropku.
    talcite::ui::marking(made, {aside - seen.at.x, -seen.at.y}, shown, shortcut_column(shown),
                      label, how.style(role() + ".key"));

    return made;
}

// Kliknięcie robi to samo, co klawisz — i **nie zabiera ogniska**. Pasek klawiszy w Midnight
// Commanderze nie odbiera go panelowi, bo po kliknięciu „Kopiuj" kopiować ma się to, co było
// zaznaczone, a nie to, co akurat dostało ognisko.
bool LegendItem::handle(ClickEvent&) {
    pick();

    return true;
}

}
