#pragma once

#include <memory>
#include <vector>

#include "talcite/node.hpp"

namespace talcite::ui {

// **Przystanki wędrówki ognisk** — wszyscy chętni na ognisko w głąb, w kolejności ułożenia. Tab
// w zasłonie chodzi dokładnie po nich, a okno niemodalne bierze pierwszego, gdy nie pamięta
// niczego. Jedno pojęcie, dotąd w dwóch kopiach: w zasłonie i w pojemniku okien.
//
// **Widżet chętny na ognisko jest liściem**: co ma w środku, rozstrzyga sam. Bez tej reguły Tab
// w oknie z listą chodziłby po każdym jej wierszu z osobna, bo wiersz też jest chętny. Samego
// `from` reguła nie dotyczy, bo zbieranie zaczyna się od jego dzieci i o jego chętność nikt tu nie
// pyta — zasłona jest chętna, a jej przystanki i tak leżą w środku.
inline void stops(const Node& from, std::vector<Node*>& into) {
    for (const std::shared_ptr<Node>& one : from.children()) {
        if (one->focusable()) {
            into.push_back(one.get());

            continue;
        }

        stops(*one, into);
    }
}

[[nodiscard]] inline std::vector<Node*> stops(const Node& from) {
    std::vector<Node*> made;

    stops(from, made);

    return made;
}

}
