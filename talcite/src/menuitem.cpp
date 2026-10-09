#include "talcite/menuitem.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

#include "talcite/encoding.hpp"
#include "talcite/menu.hpp"
#include "talcite/root.hpp"
#include "talcite/skin.hpp"

#include "marking.hpp"

namespace talcite::ui {

MenuItem::MenuItem(std::string text) : text_(std::move(text)) { role("talcite.menu"); }

MenuItem::MenuItem(std::string text, std::string hint)
    : text_(std::move(text)), hint_(std::move(hint)) {
    role("talcite.menu");
}

// Pozycja nie ma czego trzymać w środku — tak samo jak napis.
int MenuItem::capacity() const { return 0; }

// Po spacji z każdej strony i dwie między napisem a podpisem skrótu, żeby „Kopiuj" nie kleiło
// się do „F5". Wysokość zawsze jeden wiersz: pozycja menu, która się zawija, to nie pozycja.
Size MenuItem::wants(Size room) const {
    int wide = 2 + measured(text_).wide;

    if (!hint_.empty()) wide += 2 + measured(hint_).wide;

    return {std::min(wide, room.wide), std::min(1, room.high)};
}

// **Podświetlona pozycja to ta z ogniskiem.** Nie ma osobnego znacznika, bo dwa źródła tej samej
// prawdy prędzej czy później się rozjeżdżają — dokładnie z tego powodu menu nie ma pola „otwarte".
// Kreska jest niedostępna, więc sama z siebie ogniska nie weźmie i nic o niej wiedzieć nie trzeba.
//
// Podświetlenie robimy odwróceniem, a nie kolorem: działa na każdym terminalu i nie wymaga
// wymyślania barw, o które nikt nie prosił. Niedostępna pozycja gaśnie.
//
// Rola powstaje ze **rdzenia i końcówki**, tak samo jak w przycisku: rdzeń zadaje ten, kto składa
// menu, a końcówkę dokłada pozycja, bo tylko ona wie, jakie ma stany.
View MenuItem::paint(Size given, Area seen) {
    View made{seen.size};

    const Skin& how = skin();

    const Style look = how.style(!enabled()      ? role() + ".disabled"
                                      : focused() ? role() + ".focus"
                                                  : role());

    made.fill({{-seen.at.x, -seen.at.y}, given}, look);
    made.put({1 - seen.at.x, -seen.at.y}, text_, look);

    if (!hint_.empty()) {
        const int wide = measured(hint_).wide;

        made.put({given.wide - 1 - wide - seen.at.x, -seen.at.y}, hint_, look);
    }

    // Litera aktywna ma własną rolę, nakładaną na wygląd pozycji — jak w przycisku i w ptaszku.
    // Napis stoi o jedną kratkę od lewej, więc i litera jest o tę kratkę przesunięta.
    talcite::ui::marking(made, {1 - seen.at.x, -seen.at.y}, text_, shortcut_column(text_),
                      look, how.style(role() + ".key"));

    return made;
}

// Klawisz aktywny przychodzi z bazy. Pozycja robi to samo, co przy kliknięciu: najpierw staje się
// bieżącą, potem idzie wybór — bo inaczej menu wybrałoby tę, po której ostatnio chodziły strzałki.
void MenuItem::activate() {
    if (Menu* one = dynamic_cast<Menu*>(parent())) {
        one->current(*this);
        one->pick();

        return;
    }

    pick();
}

// Sam wybór to wywołanie tego, co powierzył budujący. Zamykaniem menu zajmuje się Menu, bo
// pozycja nie wie, że leży w czymś, co się otwiera.
void MenuItem::pick() {
    if (!enabled()) return;

    (void)trigger(PickEvent{});
}

bool MenuItem::handle(ClickEvent&) {
    if (!enabled()) return true;

    // Kliknięta pozycja najpierw staje się podświetloną. Inaczej Menu wybrałoby tę, po której
    // ostatnio chodziły strzałki, a nie tę pod kursorem.
    if (Menu* one = dynamic_cast<Menu*>(parent())) {
        one->current(*this);
        one->pick();

        return true;
    }

    pick();

    return true;
}

// Najechanie myszą przestawia podświetlenie, jak w Midnight Commanderze. Zdarzenie puszczamy
// dalej, bo ruch myszy nie należy do nikogo na wyłączność.
bool MenuItem::handle(MouseEvent& what) {
    if (what.press() != Press::move || !enabled()) return false;

    if (Menu* one = dynamic_cast<Menu*>(parent())) one->current(*this);

    return false;
}

}
