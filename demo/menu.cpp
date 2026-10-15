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

// A menu bar modeled on Midnight Commander. F9 opens it, the arrows move, Enter chooses, Esc
// closes, and a click works the same way. F10 quits.
class Sheet : public talcite::ui::Root {
public:
    explicit Sheet(talcite::Term& term) : term_(term) {}

protected:
    // F10 stops the terminal; every other key is reported as handled.
    bool handle(talcite::ui::KeyEvent& what) override {
        if (what.key() == talcite::ui::Code::f10) term_.stop();

        return true;
    }

private:
    talcite::Term& term_;
};

// A pane is greedy: it takes as much as it is given. The row below the bar gives its child exactly
// as much as the child wants, so content meant to fill the screen has to ask for it.
}

// A menu bar over a long scrolling page, with hot letters on titles and items and one disabled
// item.
int main() {
    demo::Hooks heard;

    talcite::Term term;

    Sheet sheet{term};

    const auto window = std::make_shared<talcite::ui::Menubar>();

    // The application gives the key that opens the menus; the library assigns none by itself.
    window->keys().add(talcite::ui::Key{talcite::ui::Code::f9});


    const auto pane = std::make_shared<talcite::ui::Pane>();
    const auto frame = std::make_shared<talcite::ui::Frame>("talcite — menu");
    const auto scroll = std::make_shared<talcite::ui::Scroll>(talcite::ui::Axis::vertical, talcite::ui::ScrollShape{true});
    const auto column = std::make_shared<talcite::ui::Column>();
    const auto said = std::make_shared<talcite::ui::Span>("F9 opens the menu, F10 quits");

    column->children().add(said);

    for (int which = 0; which < 60; ++which)
        column->children().add(std::make_shared<talcite::ui::Span>("row number " + std::to_string(which)));

    scroll->children().add(column);
    frame->children().add(scroll);
    pane->children().add(frame);
    window->content(pane);

    // A hot letter in a menu item, as in Midnight Commander: with File open, pressing "p" previews
    // and "c" copies. The letters are given separately, so the label stays plain, with no ampersand
    // convention, and the skin draws the underline through the role `talcite.menu.key`.
    const auto lettered = [](const std::shared_ptr<talcite::ui::MenuItem>& one, char32_t which) {
        one->keys().add(which);

        return one;
    };

    const auto file = window->add("File");

    // A title's letter works only while the bar is active, that is after F9, as in MC. The title
    // letters are chosen so that they do not clash with the item letters: an item lies closer to
    // the focus, so with File open, pressing "p" chooses Preview rather than opening Help.
    file->keys().add(U'l');

    file->children().add(
        lettered(demo::doing(heard, std::make_shared<talcite::ui::MenuItem>("Preview", "F3"),
                             [said] { said->text("chosen: Preview"); }),
                 U'p'));
    file->children().add(
        lettered(demo::doing(heard, std::make_shared<talcite::ui::MenuItem>("Edit", "F4"),
                             [said] { said->text("chosen: Edit"); }),
                 U'e'));
    file->children().add(
        lettered(demo::doing(heard, std::make_shared<talcite::ui::MenuItem>("Copy", "F5"),
                             [said] { said->text("chosen: Copy"); }),
                 U'c'));
    file->children().add(std::make_shared<talcite::ui::MenuSeparator>());
    file->children().add(
        lettered(demo::doing(heard, std::make_shared<talcite::ui::MenuItem>("Exit", "F10"),
                             [&term] { term.stop(); }),
                 U'x'));

    const auto view = window->add("View");

    view->keys().add(U'v');

    view->children().add(
        lettered(demo::doing(heard, std::make_shared<talcite::ui::MenuItem>("Reload", "Ctrl-R"),
                             [said] { said->text("chosen: Reload"); }),
                 U'o'));

    const auto niedostepna = std::make_shared<talcite::ui::MenuItem>("Paste", "Ctrl-V");

    // A grayed-out item does not answer its letter, just like a disabled button.
    niedostepna->keys().add(U'p');
    niedostepna->enabled(false);
    view->children().add(niedostepna);

    const auto help = window->add("Help");

    help->keys().add(U'h');

    help->children().add(demo::doing(heard, std::make_shared<talcite::ui::MenuItem>("About"),
                          [said] { said->text("talcite — a universal framework of views"); }));
    help->children().add(
        demo::doing(heard, std::make_shared<talcite::ui::MenuItem>("A very long item near the edge"),
                    [said] { said->text("chosen: the long item"); }));

    sheet.children().add(window);

    pane->focus();

    term.show(sheet);
    term.title("talcite — menu");

    term.run();

    return 0;
}
