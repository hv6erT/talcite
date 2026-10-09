#include "talcite/listitem.hpp"

#include <algorithm>
#include <utility>

#include "eliding.hpp"
#include "talcite/list.hpp"
#include "talcite/root.hpp"
#include "talcite/skin.hpp"

namespace talcite::ui {

namespace {

// Czy to jest bieżący wiersz swojej listy. Pytamy listę, bo to ona prowadzi numer — pozycja nie
// trzyma drugiego znacznika obok ogniska i obok tego numeru.
//
// **Nie jest to metoda publiczna i nie ma być.** Kto pyta o bieżący wiersz, pyta listy: `current()`.
// Pozycji do tego nie potrzeba, a druga droga do jednej prawdy to zawsze druga droga do rozjazdu.
bool current(const ListItem& one) {
    const List* up = dynamic_cast<const List*>(one.parent());

    if (up == nullptr) return false;

    return up->current() == &one;
}

}

ListItem::ListItem(std::string text) : text_(std::move(text)) { role("talcite.list"); }

// Pozycja nie ma czego trzymać w środku — tak samo jak napis i jak pozycja menu.
int ListItem::capacity() const { return 0; }

// Bez odstępów po bokach, inaczej niż w pozycji menu: w liście plików nazwa zaczyna się od pierwszej
// kratki i nie ma jej od czego odsuwać. Szerokość to sam napis — rozciąga ją lista, dając wierszowi
// cały swój prostokąt.
Size ListItem::wants(Size room) const {
    return {std::min(measured(text_).wide, room.wide), std::min(1, room.high)};
}

// Sam wybór to wywołanie tego, co powierzył budujący. O tym, że pozycja leży w czymś, co się
// przewija, nie wie i wiedzieć nie musi.
// Zdarzenie **bąbelkuje przez listę**, więc aplikacja może słuchać na niej jednym uchwytem
// zamiast wieszać wywołanie zwrotne na każdym wierszu z osobna. `Event::at()` mówi który.
void ListItem::pick() {
    if (!enabled()) return;

    (void)trigger(PickEvent{});
}

// **Bieżąca pozycja to ta z ogniskiem.** Nie ma osobnego znacznika „podświetlona", bo dwa źródła
// tej samej prawdy prędzej czy później się rozjeżdżają — dokładnie z tego powodu menu nie ma pola
// „otwarte", tylko pyta o ognisko. Dzięki temu skórka będzie miała rolę „pozycja z ogniskiem",
// a nie „pozycja z jakimś znacznikiem, który akurat znaczy to samo".
//
// **Bieżący wiersz bez ogniska ma własną rolę.** Panel Midnight Commandera nie pokazuje kursora,
// gdy jest nieaktywny, ale listbox w oknie dialogowym pokazuje go innym kolorem — i to jest
// `talcite.list.mark`. Wbudowana skórka zostawia tę rolę pustą, więc dziś nie widać nic, dokładnie jak
// dotąd; wypełni ją pierwsza skórka z barwami i **z kodu nie ubędzie ani nie przybędzie nic**.
//
// Napis dłuższy niż wiersz przycinamy wielokropkiem z prawej, jak w przycisku. Pole się przewija,
// bo w nim się pisze; wiersz listy nie ma czego przewijać, więc uczciwiej jest przyciąć.
View ListItem::paint(Size given, Area seen) {
    View made{seen.size};

    const Skin& how = skin();

    const Style look = how.style(!enabled()           ? role() + ".disabled"
                                      : focused()      ? role() + ".focus"
                                      : current(*this) ? role() + ".mark"
                                                       : role());

    made.fill({{-seen.at.x, -seen.at.y}, given}, look);
    made.put({-seen.at.x, -seen.at.y},
                elided(text_, given.wide, Elide::right, how.glyph("talcite.elide")),
                look);

    return made;
}

// Enter jest **klawiszem samej pozycji**, bo to ona ma ognisko — inaczej niż w menu, gdzie Enter
// bierze menu, bo pozycja ogniska nie ma. Wszystko inne oddajemy dalej: strzałki i klawisze stron
// należą do listy, litery i Tab do okna.
//
// `handle` biegnie **przed** uchwytami z `on<KeyEvent>`, ale ich nie zasłania: kanały są dwa
// i prawda z `handle` zamyka tylko drogę widżetów. Dopięty uchwyt zobaczy więc każdy klawisz,
// łącznie z Enterem — a Enter ogłasza się dodatkowo jako `PickEvent`.
bool ListItem::handle(KeyEvent& what) {
    if (!enabled()) return false;

    if (what.mods().ctrl || what.mods().alt) return false;

    if (what.key().code() != Code::enter) return false;

    pick();

    return true;
}

// Klik bierze ognisko i tym samym czyni pozycję bieżącą. Reszta — numer wiersza, dociągnięcie
// widoku, powiadomienie aplikacji — dzieje się w liście, do której `FocusEvent` i tak zabąbelkuje.
bool ListItem::handle(ClickEvent&) {
    // Pozycja niedostępna **przepuszcza klik wyżej**, zamiast go połykać: skoro nie odpowiada na
    // swój klawisz, to nie ma powodu, żeby odpowiadała na mysz — a lista pod spodem ma prawo
    // zrobić z tym kliknięciem, co uważa.
    if (!enabled()) return false;

    if (root() != nullptr) focus();

    return true;
}

// Dwuklik w wiersz robi to, co Enter — i po drodze też stawia na nim ognisko, bo dwuklik może
// paść na wiersz, który bieżącym jeszcze nie jest.
bool ListItem::handle(DoubleClickEvent&) {
    if (!enabled()) return false;

    if (root() != nullptr) focus();

    pick();

    return true;
}

}
