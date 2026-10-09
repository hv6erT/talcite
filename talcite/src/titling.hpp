#pragma once

#include <algorithm>
#include <string>

#include "eliding.hpp"
#include "talcite/area.hpp"
#include "talcite/shape.hpp"
#include "talcite/view.hpp"

namespace talcite::ui {

// Napis wstawiony w linię: w krawędzi ramki albo w rozdzielniku. Stoi **w jednym miejscu**, bo
// rysuje go i `Frame`, i `Line` — a dwie kopie tej samej funkcji w dwóch jednostkach to naruszenie
// zasady jednej definicji, którego konsolidator nie zgłasza. Tę pułapkę złapaliśmy już raz przy
// `HandPromise`.
//
// Napis dostaje po spacji z każdej strony, żeby nie kleił się do linii — tak robi Midnight
// Commander i tak się to po prostu czyta. Skracamy go, gdy nie mieści się w podanym miejscu.
//
// **Styl przychodzi z zewnątrz, a nie z roli.** Ten napis rysuje się w środku cudzego widżetu, więc
// to gospodarz odpytał skórkę i to on wie, jak ma wyglądać jego tytuł.
inline void along(View& into, Spot at, int much, const std::string& said, Justify justify,
                  const Style& look, char32_t mark) {
    if (said.empty() || much <= 2) return;

    const std::string whole = " " + said + " ";

    const int wide = std::min(measured(whole).wide, much);

    const int aside = much - wide;
    const int x = justify == Justify::left    ? 0
                  : justify == Justify::right ? aside
                                                   : aside / 2;

    into.put({at.x + x, at.y}, elided(whole, wide, Elide::right, mark), look);
}

}
