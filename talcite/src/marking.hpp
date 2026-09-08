#pragma once

#include <string>

#include "talcite/area.hpp"
#include "talcite/encoding.hpp"
#include "talcite/style.hpp"
#include "talcite/view.hpp"

namespace talcite::ui {

// Domalowanie **litery aktywnej** w napisie, który już stoi na obrazie. Kolumnę wskazuje
// `marking(const Keys&, const std::string&)`; tutaj zostaje samo znalezienie znaku, który na tej
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
inline void marking(View& into, Spot at, const std::string& shown, int which, const Style& how) {
    if (which < 0) return;

    std::size_t where = 0;
    int wide = 0;

    while (where < shown.size()) {
        const std::size_t began = where;
        const char32_t sign = letter(shown, where);

        if (began == where) return;

        if (wide == which) {
            into.string({at.x + which, at.y}, spelled(sign), how);

            return;
        }

        wide += widely(sign);
    }
}

}
