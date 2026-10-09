#pragma once

#include <string>

#include "talcite/area.hpp"
#include "talcite/encoding.hpp"
#include "talcite/shape.hpp"
#include "talcite/view.hpp"

namespace talcite::ui {

// Domalowanie **litery aktywnej** w napisie, który już stoi na obrazie. Kolumnę wskazuje
// `shortcut_column(const Keys&, const std::string&)`; tutaj zostaje samo znalezienie znaku, który na tej
// kolumnie siedzi, i przemalowanie go innym wyglądem.
//
// Rozdzielenie na dwie części nie jest kaprysem: kolumnę trzeba znać **przed** rysowaniem, bo
// bywa potrzebna do rozplanowania napisu, a przemalować można dopiero **po**. Widżet i tak wie,
// o ile kratek przesunął napis od lewej, więc przesunięcie podaje w `at`.
//
// Stało to dotąd przepisane w przycisku, w pozycji menu i w polu wyboru — trzy egzemplarze tej
// samej pętli po znakach napisu. Czwarty i piąty egzemplarz miały właśnie dojść w pasku klawiszy
// i w pasku menu; to jest ten moment, w którym kopiowanie przestaje się opłacać.
//
// Chodzimy po **kratkach**, nie po bajtach, bo napis jest w UTF-8, a znaki szerokie zajmują dwie
// kratki. Napis musi być ten sam, który poszedł na obraz — czyli **już przycięty**: kolumna
// policzona po całości trafiłaby w wielokropek.
//
// **Litera aktywna zmienia pismo i atrybuty, nigdy tła.** Rola `.key` wchodzi na wygląd napisu
// bez swojego `background`: skórki przeniesione z Midnight Commandera mają w `.key` parę pismo–tło
// (tam `menuhot` i `dhotnormal` to `fg;bg`), a MC ma na literę w pozycji podświetlonej **osobną**
// rolę (`menuhotsel`, `dhotfocus`), której u nas nie ma. Z tłem z `.key` litera w podświetlonym
// tytule stała na tle paska, jak dziura w podświetleniu — widać to było w każdej z dwudziestu
// dziewięciu skórek, bo w każdej tło `.key` równa się tłu zwykłej pozycji. Tło bierze więc to,
// na czym litera stoi; pismo i pogrubienie czy podkreślenie idą z `.key`.
inline void marking(View& into, Spot at, const std::string& shown, int which, Style look,
                    Style key) {
    if (which < 0) return;

    key.background = Color{};

    const Style how = Style::merge(look, key);

    std::size_t where = 0;
    int wide = 0;

    while (where < shown.size()) {
        const std::size_t began = where;
        const char32_t sign = utf8::decode(shown, where);

        if (began == where) return;

        if (wide == which) {
            into.put({at.x + which, at.y}, utf8::encode(sign), how);

            return;
        }

        wide += width(sign);
    }
}

}
