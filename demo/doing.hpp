#pragma once

#include <functional>
#include <memory>
#include <utility>

#include "talcite/event.hpp"
#include "talcite/node.hpp"
#include "talcite/modal.hpp"

namespace demo {

// Czynność przypięta do naciśnięcia.
//
// Biblioteka ma **jeden** mechanizm zawiadamiania — `on<PickEvent>` — i celowo nie daje skrótu,
// bo skrót w bibliotece stałby się drugim mechanizmem. Tu, po stronie aplikacji, skrót jest na
// miejscu: mieści wołanie w jednej linijce, a pod spodem jest tym samym uchwytem, który mógłby
// powiesić ktokolwiek.
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

// Zamykanie zasłony Escapem.
//
// Biblioteka tego **nie robi**: zamknięcie okna jest czynnością, a czynności, której wołający nie
// może odwołać, `handle` wykonywać nie ma prawa. Tu, po stronie aplikacji, decyzja jest nasza — i to
// jest dokładnie ta jedna linijka, którą płaci się za tę regułę.
template <typename One>
std::shared_ptr<One> closing(std::shared_ptr<One> one) {
    const std::weak_ptr<talcite::ui::Modal> weak = one;

    (void)one->template on<talcite::ui::KeyEvent>([weak](talcite::ui::KeyEvent& what) {
        if (what.key().code() != talcite::ui::Code::escape) return false;

        if (const std::shared_ptr<talcite::ui::Modal> mine = weak.lock()) mine->close();

        return true;
    });

    return one;
}

}
