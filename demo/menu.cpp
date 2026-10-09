#include <memory>
#include <string>

#include "talcite/column.hpp"
#include "talcite/frame.hpp"
#include "talcite/menu.hpp"
#include "talcite/menuitem.hpp"
#include "talcite/menuseparator.hpp"
#include "talcite/menubar.hpp"
#include "talcite/pane.hpp"
#include "talcite/span.hpp"
#include "talcite/term.hpp"
#include "talcite/scroll.hpp"

#include "doing.hpp"

namespace {

// Pasek menu wzorowany na Midnight Commanderze. F9 otwiera, strzałki chodzą, Enter wybiera,
// Esc zamyka, klik działa tak samo. F10 kończy.
class Sheet : public talcite::ui::Root {
public:
    explicit Sheet(talcite::Term& term) : term_(term) {}

protected:
    bool handle(talcite::ui::KeyEvent& what) override {
        if (what.key() == talcite::ui::Code::f10) term_.stop();

        return true;
    }

private:
    talcite::Term& term_;
};

// Panel jest łakomy: bierze tyle, ile mu dadzą. Wiersz pod paskiem daje dziecku dokładnie
// tyle, ile ono chce, więc treść, która ma wypełnić ekran, musi o niego poprosić.
}

int main() {
    talcite::Term term;

    Sheet sheet{term};

    const auto window = std::make_shared<talcite::ui::Menubar>();

    // **Klawisz otwierający podaje aplikacja** — biblioteka żadnego sobie nie przypisuje.
    window->keys().add(talcite::ui::Key{talcite::ui::Code::f9});


    const auto pane = std::make_shared<talcite::ui::Pane>();
    const auto frame = std::make_shared<talcite::ui::Frame>("talcite — menu");
    const auto scroll = std::make_shared<talcite::ui::Scroll>(talcite::ui::Axis::vertical, talcite::ui::ScrollShape{true});
    const auto column = std::make_shared<talcite::ui::Column>();
    const auto said = std::make_shared<talcite::ui::Span>("F9 otwiera menu, F10 kończy");

    column->children().add(said);

    for (int which = 0; which < 60; ++which)
        column->children().add(std::make_shared<talcite::ui::Span>("wiersz numer " + std::to_string(which)));

    scroll->children().add(column);
    frame->children().add(scroll);
    pane->children().add(frame);
    window->content(pane);

    // **Litera aktywna w pozycji menu**, jak w Midnight Commanderze: przy otwartym „Plik"
    // wciśnięcie „p" podejrzy, „k" skopiuje. Litery podaje się osobno — napis zostaje czysty,
    // bez umów o ampersandzie — a podkreślenie robi skórka rolą `talcite.menu.key`.
    const auto lettered = [](const std::shared_ptr<talcite::ui::MenuItem>& one, char32_t which) {
        one->keys().add(which);

        return one;
    };

    const auto file = window->add("Plik");

    // **Litera tytułu działa dopiero przy czynnym pasku**, czyli po F9 — tak jak w MC. Litery
    // tytułów dobrano tak, żeby nie ścierały się z literami pozycji: pozycja leży bliżej ogniska,
    // więc przy otwartym „Plik" wciśnięcie „w" wybiera „Wyjście", a nie otwiera „Widok".
    file->keys().add(U'l');

    file->children().add(
        lettered(demo::doing(std::make_shared<talcite::ui::MenuItem>("Podejrzyj", "F3"),
                             [said] { said->text("wybrano: Podejrzyj"); }),
                 U'p'));
    file->children().add(
        lettered(demo::doing(std::make_shared<talcite::ui::MenuItem>("Popraw", "F4"),
                             [said] { said->text("wybrano: Popraw"); }),
                 U'o'));
    file->children().add(
        lettered(demo::doing(std::make_shared<talcite::ui::MenuItem>("Kopiuj", "F5"),
                             [said] { said->text("wybrano: Kopiuj"); }),
                 U'k'));
    file->children().add(std::make_shared<talcite::ui::MenuSeparator>());
    file->children().add(
        lettered(demo::doing(std::make_shared<talcite::ui::MenuItem>("Wyjście", "F10"),
                             [&term] { term.stop(); }),
                 U'w'));

    const auto view = window->add("Widok");

    view->keys().add(U'd');

    view->children().add(
        lettered(demo::doing(std::make_shared<talcite::ui::MenuItem>("Odśwież", "Ctrl-R"),
                             [said] { said->text("wybrano: Odśwież"); }),
                 U'o'));

    const auto niedostepna = std::make_shared<talcite::ui::MenuItem>("Wklej", "Ctrl-V");

    // Wyszarzona pozycja **nie odpowiada na swoją literę** — tak samo jak wyłączony przycisk.
    niedostepna->keys().add(U'w');
    niedostepna->enabled(false);
    view->children().add(niedostepna);

    const auto help = window->add("Pomoc");

    help->keys().add(U'm');

    help->children().add(demo::doing(std::make_shared<talcite::ui::MenuItem>("O programie"),
                          [said] { said->text("talcite — powszechna rama widoków"); }));
    help->children().add(
        demo::doing(std::make_shared<talcite::ui::MenuItem>("Bardzo długa pozycja przy krawędzi"),
                    [said] { said->text("wybrano: długą pozycję"); }));

    sheet.children().add(window);

    pane->focus();

    term.show(sheet);
    term.title("talcite — menu");

    term.run();

    return 0;
}
