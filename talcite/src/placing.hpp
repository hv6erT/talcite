#pragma once

#include "talcite/node.hpp"

namespace talcite::ui {

// **Zdarzenie myszy niesie kratkę ekranu, a `area().at` mówi tylko o jednym piętrze.** Odjęcie
// samego `area().at` daje dobry wynik wyłącznie wtedy, gdy suma przesunięć wszystkich przodków
// jest zerowa — czyli w próbie, gdzie widżet stoi wprost pod korzeniem. W dialogu, w ramce albo
// w wierszu klik trafiał o tyle kratek obok, ile wynosiło przesunięcie przodków.
//
// Przejście w górę to ta sama arytmetyka, którą robi `Root::under` schodząc w dół i którą robi
// `Rolled::box`, licząc pozycję kotwicy. Głębokość drzewa jest rzędu kilku, więc kosztuje to tyle,
// co nic — a liczyć trzeba przy zdarzeniu, nie przy klatce, bo drzewo mogło się w międzyczasie
// przesunąć.
[[nodiscard]] inline Spot own(const Node& one, Spot where) {
    for (const Node* at = &one; at != nullptr; at = at->parent())
        where = where - at->area().at;

    return where;
}

}
