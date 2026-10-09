#pragma once

#include <algorithm>

#include "talcite/area.hpp"
#include "talcite/node.hpp"
#include "talcite/root.hpp"

namespace talcite::ui {

// **Podążanie przewijaka za ogniskiem.** `Scroll` robi to na każdej otwartej osi: po rozstawieniu dzieci
// pytają węzeł z ogniskiem o jego prostokąt w treści (`Node::area_in`) i dosuwają przewinięcie.
// Nagłówek wewnętrzny, jak `spool.hpp`: API biblioteki tego nie potrzebuje.

// Ile razy najwyżej wolno w jednej klatce dosunąć i narysować od nowa: każdy nawrót rozstawia
// jeden poziom głębiej, a drzewa głębszego niż to nikt tu nie składa.
inline constexpr int passes = 8;

// Węzeł w ognisku korzenia, o ile przewijak w ogóle stoi w drzewie.
[[nodiscard]] inline const Node* beheld(const Node& scroll) {
    const Root* top = scroll.root();

    return top != nullptr ? top->focused_node() : nullptr;
}

// Przewinięcie, przy którym odcinek [at, at + much) treści mieści się w oknie o długości `window`
// — dosunięty do bliższego brzegu; gdy dłuższy niż okno, wygrywa początek. Jedna oś naraz. Przycięcie na końcu jest strażą, nie regułą: dla węzła leżącego w treści
// wynik i tak mieści się w [0, whole - window], ale prostokąt węzła z pojemnika, którego ta klatka
// nie rozstawiała, jest z poprzedniej klatki i może wystawać za treść — a przewinięcie za koniec
// pokazałoby pustkę do następnej klatki. Mutant bez przycięcia przeżył zestaw; kod zostaje z tego
// powodu.
[[nodiscard]] inline int followed(int shift, int at, int much, int window, int whole) {
    int want = shift;

    if (at + much > want + window) want = at + much - window;
    if (at < want) want = at;

    return std::clamp(want, 0, std::max(whole - window, 0));
}

}
