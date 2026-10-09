#include "talcite/windows.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "talcite/root.hpp"
#include "talcite/window.hpp"

#include "stops.hpp"

namespace talcite::ui {

// **Okna niemodalne w jednym miejscu**, jak zasłony w `Modals`: otwieranie, zamykanie i bieżące
// okno są wspólne w `Layers`. Różnice wobec zasłon biorą się z tego, że tu nie ma modalności:
//
// - **kolejność ułożenia to nie kolejność otwarcia.** Ułożenie mówi, co na czym leży, i zmienia je
//   każde podniesienie; lista warstw trzyma kolejność otwarcia i to po niej chodzą `next`
//   i `previous` — inaczej „następne okno" skakałoby po historii patrzenia, a nie po plikach,
//   i dwa naciśnięcia wracałyby w to samo miejsce;
// - **ognisko pamięta okno, nie pojemnik.** Powrót do okna stawia kursor tam, gdzie był, a nie na
//   pierwszym chętnym — plik, po którym się chodziło, ma zostać tam, gdzie się go zostawiło.

namespace {

[[nodiscard]] bool inside(const Node* one, const Node& under) {
    for (; one != nullptr; one = one->parent())
        if (one == &under) return true;

    return false;
}

// Sąsiad bieżącego okna w kolejności otwarcia, w kółko. Jedno okno albo żadne — nie ma sąsiada.
[[nodiscard]] Window* beside(const std::vector<std::shared_ptr<Layer>>& all, const Window* now, int by) {
    if (all.size() < 2) return nullptr;

    const auto at = std::find_if(all.begin(), all.end(),
                                 [now](const std::shared_ptr<Layer>& mine) { return mine.get() == now; });

    if (at == all.end()) return nullptr;

    const auto much = static_cast<std::ptrdiff_t>(all.size());
    const std::ptrdiff_t which = ((at - all.begin()) + by % much + much) % much;

    return static_cast<Window*>(all[static_cast<std::size_t>(which)].get());
}

}

void Windows::open(std::shared_ptr<Window> given) {
    Window* one = given.get();

    adopt(std::move(given));

    raise(*one);
}

// Wszystko, co pojemnik trzyma, jest oknem — przyjmuje je tylko `open` — więc rzutowanie jest pewne.
Window* Windows::current() const { return static_cast<Window*>(top()); }

// Podniesienie to **przeniesienie na koniec dzieci** — warstwa jest ich kolejnością i niczym
// więcej. Ognisko zapamiętujemy w oknie, **w którym ono siedzi**, zanim zejdzie z drzewa razem
// z przepinanym węzłem; potem oddajemy je temu, co podnoszone pamięta, a gdy nie pamięta nic (albo
// tamten węzeł już zszedł), pierwszemu chętnemu w środku.
//
// Okno szukamy po ognisku, a nie jako bieżące: przy `open` nowe okno stoi już na wierzchu, zanim
// dojdzie tutaj, więc „bieżące" byłoby nim samym i pamięć poprzedniego przepadałaby — tak było
// w pierwszej wersji, a próba tego nie widziała, bo pamiętany węzeł był zarazem pierwszym chętnym.
void Windows::raise(Window& one) {
    const auto at = std::find_if(opened().begin(), opened().end(),
                                 [&one](const std::shared_ptr<Layer>& mine) { return mine.get() == &one; });

    if (at == opened().end()) throw std::invalid_argument("talcite::ui::Windows: not mine");

    if (Root* top = root(); top != nullptr)
        if (Node* was = top->focused_node(); was != nullptr)
            for (const std::shared_ptr<Layer>& mine : opened())
                if (inside(was, *mine)) remember(*mine, was);

    const std::shared_ptr<Layer> kept = *at;

    if (current() != &one) {
        (void)children().remove(&one);
        children().add(kept);
    }

    if (root() == nullptr) return;

    if (const std::shared_ptr<Node> back = remembered(one); back && back->root() != nullptr) {
        back->focus();

        return;
    }

    const std::vector<Node*> these = stops(one);

    if (!these.empty()) these.front()->focus();
}

// Po zamknięciu ognisko idzie do nowego wierzchniego okna — z jego własną pamięcią, jak przy każdym
// podniesieniu. Pamięć zamkniętego nie ma tu nic do powiedzenia: w niemodalnych oknach ognisko
// wraca do okna, a nie do tego, kto je miał przed otwarciem.
void Windows::restore(std::weak_ptr<Node>) {
    if (Window* now = current(); now != nullptr) raise(*now);
}

// Po kolei otwarcia, w kółko — jak `Window → Next` w mcedicie.
void Windows::next() {
    if (Window* one = beside(opened(), current(), 1)) raise(*one);
}

void Windows::previous() {
    if (Window* one = beside(opened(), current(), -1)) raise(*one);
}

}
