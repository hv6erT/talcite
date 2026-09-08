#pragma once

#include <functional>
#include <memory>
#include <utility>

#include "talcite/event.hpp"
#include "talcite/node.hpp"

namespace proving {

// Czynność przypięta do naciśnięcia.
//
// Ten sam skrót, co w demach: biblioteka ma **jeden** mechanizm zawiadamiania i celowo nie daje
// skrótu, bo skrót w bibliotece stałby się drugim mechanizmem. Próba, która chce tylko policzyć
// naciśnięcia, nie musi za każdym razem rozpisywać uchwytu.
//
// Oddaje ten sam wskaźnik, żeby dało się to wpleść w miejsce, w którym widżet powstaje.
template <typename One>
std::shared_ptr<One> doing(std::shared_ptr<One> one, std::function<void()> what) {
    (void)one->template on<talcite::ui::PickEvent>(
        [what = std::move(what)](talcite::ui::PickEvent&) {
            what();

            return true;
        });

    return one;
}

}
