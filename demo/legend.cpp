#include <memory>
#include <string>

#include "talcite/column.hpp"
#include "talcite/frame.hpp"
#include "talcite/legenditem.hpp"
#include "talcite/legendwindow.hpp"
#include "talcite/menu.hpp"
#include "talcite/menuitem.hpp"
#include "talcite/menuline.hpp"
#include "talcite/menuwindow.hpp"
#include "talcite/pane.hpp"
#include "talcite/span.hpp"
#include "talcite/term.hpp"
#include "talcite/vscroll.hpp"

#include "doing.hpp"

namespace {

class Sheet : public talcite::ui::Root {
public:
    explicit Sheet(talcite::Term& term) : term_(term) {}

protected:
    bool react(talcite::ui::KeyEvent& what) override {
        if (what.key() == talcite::ui::Code::f10) term_.stop();

        return true;
    }

private:
    talcite::Term& term_;
};

}

// Ekran złożony jak w Midnight Commanderze: pasek menu na górze, treść w środku, legenda klawiszy
// na dole. **Legenda jest oknem zewnętrznym, a pasek menu siedzi w niej** — dzięki temu rozwinięte
// menu dostaje wiersze do przedostatniego i nie ma jak zamalować paska klawiszy.
int main() {
    talcite::Term term;

    Sheet sheet{term};

    const auto legend = std::make_shared<talcite::ui::LegendWindow>();
    const auto menus = std::make_shared<talcite::ui::MenuWindow>();

    // **Klawisz otwierający podaje aplikacja** — biblioteka żadnego sobie nie przypisuje.
    menus->keys().add(talcite::ui::Key{talcite::ui::Code::f9});


    const auto pane = std::make_shared<talcite::ui::Pane>();
    const auto frame = std::make_shared<talcite::ui::Frame>("talcite — legenda");
    const auto scroll = std::make_shared<talcite::ui::VScroll>(talcite::ui::ScrollStyle{true});
    const auto column = std::make_shared<talcite::ui::Column>();
    const auto said =
        std::make_shared<talcite::ui::Span>("Klawisze funkcyjne i klikanie w pola robią to samo");

    column->children().add(said);

    for (int which = 0; which < 60; ++which)
        column->children().add(std::make_shared<talcite::ui::Span>("wiersz numer " + std::to_string(which)));

    scroll->children().add(column);
    frame->children().add(scroll);
    pane->children().add(frame);
    menus->body()->children().add(pane);
    legend->body()->children().add(menus);

    const auto file = menus->add("Plik");

    file->children().add(demo::doing(std::make_shared<talcite::ui::MenuItem>("Kopiuj", "F5"),
                          [said] { said->text("z menu: Kopiuj"); }));
    file->children().add(std::make_shared<talcite::ui::MenuLine>());
    file->children().add(demo::doing(std::make_shared<talcite::ui::MenuItem>("Wyjście", "F10"),
                          [&term] { term.stop(); }));

    (void)menus->add("Widok");

    const auto told = [said](std::string what) {
        return [said, what = std::move(what)] { said->text("naciśnięto: " + what); };
    };

    (void)demo::doing(legend->add(talcite::ui::Code::f1, "Pomoc"), told("Pomoc"));
    (void)demo::doing(legend->add(talcite::ui::Code::f2, "Menu"), told("Menu"));
    (void)demo::doing(legend->add(talcite::ui::Code::f3, "Podgl"), told("Podgląd"));
    (void)demo::doing(legend->add(talcite::ui::Code::f4, "Edycja"), told("Edycja"));
    const auto kopiuj = demo::doing(legend->add(talcite::ui::Code::f5, "Kopiuj"), told("Kopiuj"));
    (void)demo::doing(legend->add(talcite::ui::Code::f6, "PrzNaz"), told("Przenieś"));
    (void)demo::doing(legend->add(talcite::ui::Code::f7, "NwKat"), told("Nowy katalog"));

    // Pole niedostępne: etykietka widoczna, nazwa przygaszona, klawisz milczy.
    const auto gone = demo::doing(legend->add(talcite::ui::Code::f8, "Usuń"), told("Usuń"));

    gone->able(false);

    // F9 połknie pasek menu, bo stoi bliżej ogniska. Kliknięcie w to pole ma jednak zrobić to samo,
    // co klawisz — więc czyn otwiera menu wprost.
    (void)demo::doing(legend->add(talcite::ui::Code::f9, "MenuGr"), [menus] {
        if (!menus->menus().empty()) menus->menus().front()->focus();
    });

    (void)demo::doing(legend->add(talcite::ui::Code::f10, "Wyjdź"), [&term] { term.stop(); });

    // **Klawisz-litera zastępuje cyfrę podkreśleniem w nazwie.** Pole ma zbiór klawiszy, więc
    // „Kopiuj" odpowiada i na F5, i na „k" — a że litera stoi w nazwie, etykietka z boku odpada.
    kopiuj->keys().add(U'k');

    sheet.children().add(legend);

    pane->focus();

    term.onto(sheet);
    term.title("talcite — legenda");

    term.run();

    return 0;
}
