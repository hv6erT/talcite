#pragma once

#include "talcite/node.hpp"

namespace talcite::ui {

class Menu;

// **Tytuł w pasku — osobny węzeł od listy, którą otwiera.** Przedtem jedno `Menu` zajmowało dwa
// nieprzyległe prostokąty: pas tytułu w wierszu zerowym i pudełko pod nim, z polem `aside_`
// wpisywanym przez pasek tuż przed malowaniem. Jedyny nie-prostokąt w bibliotece. Teraz tytuł
// stoi w rzędzie tytułów, a lista jest dzieckiem stosu paska — dwa prostokąty w dwóch miejscach
// drzewa, każdy wie, gdzie jest.
//
// Tytuł nie ma własnych klawiszy: pyta `menu.keys()`, więc litera aktywna dodana do menu działa
// bez zmian u wołającego. Ognisko na tytule to pasek **czynny, ale zwinięty**.
class MenuTitle : public Node {
public:
    explicit MenuTitle(Menu& menu);

    [[nodiscard]] Menu& menu() const { return menu_; }

    [[nodiscard]] Size wants(Size room) const override;

    [[nodiscard]] bool focusable() const override;

protected:
    View paint(Size given, Area seen) override;

    bool handle(KeyEvent& what) override;

    bool handle(ShortcutEvent& what) override;

    bool handle(ClickEvent& what) override;

private:
    Menu& menu_;
};

}
