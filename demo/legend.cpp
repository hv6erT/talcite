#include <memory>
#include <string>

#include "talcite/column.hpp"
#include "talcite/frame.hpp"
#include "talcite/legenditem.hpp"
#include "talcite/legend.hpp"
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

}

// A screen put together as in Midnight Commander: the menu bar at the top, the content in the
// middle, the key legend at the bottom. The legend is the outer window and the menu bar sits inside
// it, so an unfolded menu gets the rows down to the last but one and has no way to paint over the
// key bar.
int main() {
    talcite::Term term;

    Sheet sheet{term};

    const auto legend = std::make_shared<talcite::ui::Legend>();
    const auto menus = std::make_shared<talcite::ui::Menubar>();

    // The application gives the key that opens the menus; the library assigns none by itself.
    menus->keys().add(talcite::ui::Key{talcite::ui::Code::f9});


    const auto pane = std::make_shared<talcite::ui::Pane>();
    const auto frame = std::make_shared<talcite::ui::Frame>("talcite — legend");
    const auto scroll = std::make_shared<talcite::ui::Scroll>(talcite::ui::Axis::vertical, talcite::ui::ScrollShape{true});
    const auto column = std::make_shared<talcite::ui::Column>();
    const auto said =
        std::make_shared<talcite::ui::Span>("Function keys and clicking the fields do the same thing");

    column->children().add(said);

    for (int which = 0; which < 60; ++which)
        column->children().add(std::make_shared<talcite::ui::Span>("row number " + std::to_string(which)));

    scroll->children().add(column);
    frame->children().add(scroll);
    pane->children().add(frame);
    menus->content(pane);
    legend->content(menus);

    const auto file = menus->add("File");

    file->children().add(demo::doing(std::make_shared<talcite::ui::MenuItem>("Copy", "F5"),
                          [said] { said->text("from the menu: Copy"); }));
    file->children().add(std::make_shared<talcite::ui::MenuSeparator>());
    file->children().add(demo::doing(std::make_shared<talcite::ui::MenuItem>("Exit", "F10"),
                          [&term] { term.stop(); }));

    (void)menus->add("View");

    const auto told = [said](std::string what) {
        return [said, what = std::move(what)] { said->text("pressed: " + what); };
    };

    (void)demo::doing(legend->add(talcite::ui::Code::f1, "Help"), told("Help"));
    (void)demo::doing(legend->add(talcite::ui::Code::f2, "Menu"), told("Menu"));
    (void)demo::doing(legend->add(talcite::ui::Code::f3, "View"), told("View"));
    (void)demo::doing(legend->add(talcite::ui::Code::f4, "Edit"), told("Edit"));
    const auto kopiuj = demo::doing(legend->add(talcite::ui::Code::f5, "Copy"), told("Copy"));
    (void)demo::doing(legend->add(talcite::ui::Code::f6, "RenMov"), told("Move"));
    (void)demo::doing(legend->add(talcite::ui::Code::f7, "Mkdir"), told("New directory"));

    // A disabled field: the tag is visible, the name is dimmed, and the key is silent.
    const auto gone = demo::doing(legend->add(talcite::ui::Code::f8, "Delete"), told("Delete"));

    gone->enabled(false);

    // F9 is swallowed by the menu bar, because it stands closer to the focus. A click on this field
    // has to do the same as the key, though, so the action opens the menu directly.
    (void)demo::doing(legend->add(talcite::ui::Code::f9, "PullDn"), [menus] {
        if (!menus->menus().empty()) menus->menus().front()->focus();
    });

    (void)demo::doing(legend->add(talcite::ui::Code::f10, "Quit"), [&term] { term.stop(); });

    // A letter key is underlined in the name. A field has a set of keys, so the copy field answers
    // both F5 and "c", and since the letter occurs in "Copy", it is underlined there. The tag "5"
    // at the side stays, because the field was registered with F5: the bar reads as a row of
    // numbers. Only a field registered with a letter loses its tag to the underline.
    kopiuj->keys().add(U'c');

    sheet.children().add(legend);

    pane->focus();

    term.show(sheet);
    term.title("talcite — legend");

    term.run();

    return 0;
}
