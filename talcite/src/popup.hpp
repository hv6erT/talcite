#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "talcite/modal.hpp"

namespace talcite::ui {

class Modals;

// **Zasłona, która stawia pudełko pod wskazanym węzłem.** Przezroczystość, modalność, cień i powrót
// ogniska ma po `Modal` i niczego z tego nie powtarza.
//
// Escape'a bierze **sama**, bo `Modal` przestała go brać: zamykanie oknem, którego wołający nie może
// odwołać, jest decyzją za niego. Tutaj tej wątpliwości nie ma — rozwiniętej listy nikt z zewnątrz
// nie składa ani nie widzi, jest w całości widżetem biblioteki, tak samo jak strzałki w menu.
//
// Nie wie, czym jest wybór, lista ani historia. Dostaje węzeł, pod którym ma stanąć, i tyle —
// dlatego posłuży każdemu, kto potrzebuje czegoś rozwiniętego pod sobą, bez ani jednej gałęzi
// „a jeśli to akurat wybór".
class Rolled : public Modal {
public:
    explicit Rolled(std::weak_ptr<Node> under);

protected:
    [[nodiscard]] Area seat(Size given) const override;

    bool handle(KeyEvent& what) override;

private:
    std::weak_ptr<Node> under_;
};

// Najbliższe okno zasłon w górę drzewa. Ten sam chwyt, którym pozycja menu znajduje swoje menu,
// a zasłona swoje okno — tylko o piętro dalej, bo między widżetem a oknem stoi zwykle formularz.
[[nodiscard]] Modals* housing(Node* from);

// **Rozwijana lista jako część widżetu.** Wybór i historia miały każde własną kopię tego samego:
// liczbę wierszy, słabe wskazanie na rozwiniętą zasłonę, `opened`, `open`, `close` i niemal ten sam
// uchwyt `PickEvent`. Teraz trzymają jeden `Popup` i różnią się wyłącznie tym, skąd biorą napisy
// i co robią z wybranym wierszem.
//
// **Rozwinięta lista należy do okna, nie do nas.** Ono ją trzyma, ono ją zamyka Escapem, ono zwraca
// ognisko. Trzymając ją mocno, zostawalibyśmy z martwym uchwytem po każdym zamknięciu nie naszą
// ręką; słabe wskazanie mówi dokładnie tyle, ile jest prawdą — „ta jest moja, dopóki istnieje".
//
// Obiekt nie jest węzłem i żyje w widżecie, pod którym rozwija listę — widżet trzyma go przez
// `unique_ptr`, bo nagłówek publiczny nie może pokazać tej klasy.
class Popup {
public:
    explicit Popup(Node& under) : under_(&under) {}

    // Ile wierszy widać naraz. Dłuższa lista dostaje suwak; mniej niż jednego nie ma sensu.
    [[nodiscard]] int rows() const { return rows_; }

    void rows(int given) { rows_ = std::max(given, 1); }

    [[nodiscard]] bool opened() const { return !rolled_.expired(); }

    void close();

    void open(const std::vector<std::string>& among, std::size_t which,
              std::function<void(std::size_t)> took);

private:
    Node* under_;
    std::weak_ptr<Modal> rolled_;
    int rows_ = 8;
};

}
