#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "talcite/veil.hpp"

namespace talcite::ui {

class List;
class VeilWindow;

// **Zasłona, która stawia pudełko pod wskazanym węzłem.** Przezroczystość, modalność, cień i powrót
// ogniska ma po `Veil` i niczego z tego nie powtarza.
//
// Escape'a bierze **sama**, bo `Veil` przestała go brać: zamykanie oknem, którego wołający nie może
// odwołać, jest decyzją za niego. Tutaj tej wątpliwości nie ma — rozwiniętej listy nikt z zewnątrz
// nie składa ani nie widzi, jest w całości widżetem biblioteki, tak samo jak strzałki w menu.
//
// Nie wie, czym jest wybór, lista ani historia. Dostaje węzeł, pod którym ma stanąć, i tyle —
// dlatego posłuży każdemu, kto potrzebuje czegoś rozwiniętego pod sobą, bez ani jednej gałęzi
// „a jeśli to akurat wybór".
class Rolled : public Veil {
public:
    explicit Rolled(std::weak_ptr<Node> under);

protected:
    [[nodiscard]] Area box(Size given) const override;

    bool react(KeyEvent& what) override;

private:
    std::weak_ptr<Node> under_;
};

// Najbliższe okno zasłon w górę drzewa. Ten sam chwyt, którym pozycja menu znajduje swoje menu,
// a zasłona swoje okno — tylko o piętro dalej, bo między widżetem a oknem stoi zwykle formularz.
[[nodiscard]] VeilWindow* housing(Node* from);

struct Rolls {
    std::shared_ptr<Veil> veil;
    std::shared_ptr<List> list;
};

// Złożenie i otwarcie rozwiniętej listy: ramka, lista z pozycjami, zasłona pod widżetem, wpis do
// okna. Wołający dokłada już tylko **własny** uchwyt na `PickEvent` — bo tylko on wie, co z wyborem
// zrobić. Pusty wynik znaczy „nie ma gdzie tego otworzyć".
//
// Wspólne dla wyboru i historii, bo obie robią z tym dokładnie to samo i różnią się wyłącznie tym,
// skąd biorą napisy.
[[nodiscard]] Rolls rolling(Node& under, const std::vector<std::string>& among, std::size_t which,
                            int most);

}
