#pragma once

#include <memory>
#include <utility>

#include "talcite/node.hpp"

namespace talcite::ui {

// **Gniazdo na treść aplikacji.** Zasłony, legenda i pasek menu stawiają pod sobą wiersz, w który
// aplikacja wkłada to, co ma stać pod nimi. Dotąd każde oddawało ten wiersz na zewnątrz i wołający
// pisał `body()->children().add(x)` — sięgał przez cudzy węzeł do jego dzieci, a wiersz przyjmował
// dowolnie wiele treści, choć zawsze stała w nim jedna. Teraz treść jest jedna i ustawia się ją
// wprost: `content(x)`. Wiersz zostaje w środku jako gniazdo, bo to on daje treści całe miejsce.
[[nodiscard]] inline Node* held(const Node& slot) {
    return slot.children().empty() ? nullptr : slot.children().front().get();
}

// Nowa treść wypiera starą, a pusty wskaźnik opróżnia gniazdo. Ta sama treść drugi raz nie robi
// nic — wyjęcie i włożenie z powrotem zabrałoby jej ognisko.
inline void hold(Node& slot, std::shared_ptr<Node> given) {
    if (given.get() == held(slot)) return;

    slot.children().clear();

    if (given != nullptr) slot.children().add(std::move(given));
}

}
