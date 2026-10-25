#include <memory>
#include <string>

#include "talcite/box.hpp"
#include "talcite/button.hpp"
#include "talcite/column.hpp"
#include "talcite/dialog.hpp"
#include "talcite/field.hpp"
#include "talcite/focusholder.hpp"
#include "talcite/frame.hpp"
#include "talcite/keybar.hpp"
#include "talcite/keybaritem.hpp"
#include "talcite/modals.hpp"
#include "talcite/row.hpp"
#include "talcite/span.hpp"
#include "talcite/tab.hpp"
#include "talcite/tabs.hpp"
#include "talcite/term.hpp"
#include "talcite/tick.hpp"

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

// A tab with its name, its key and its page.
std::shared_ptr<talcite::ui::Tab> tabbed(const std::string& name, char32_t key, std::shared_ptr<talcite::ui::Node> page) {
    const auto made = std::make_shared<talcite::ui::Tab>(name);

    made->keys().add(key);
    made->children().add(std::move(page));

    return made;
}

// A window of three pages under a strip of tabs. Each page has its own fields and buttons, and only
// the page shown answers keys: the letters of a hidden page's buttons do nothing. A click on a
// name, its letter, or Ctrl+PgUp and Ctrl+PgDn change the page; the window keeps the size of the
// largest one.
std::shared_ptr<demo::Kept<talcite::ui::Dialog>> settings(const std::shared_ptr<talcite::ui::Span>& told) {
    const auto made = std::make_shared<demo::Kept<talcite::ui::Dialog>>();
    const auto frame = std::make_shared<talcite::ui::Frame>("Settings", talcite::ui::FrameShape{talcite::ui::Border::twin});

    const auto general = std::make_shared<talcite::ui::Column>();
    const auto name = std::make_shared<talcite::ui::Field>(30, "talcite");

    general->children().add({std::make_shared<talcite::ui::Span>("Name:"), name,
                              std::make_shared<talcite::ui::Tick>("Start at login")});

    const auto look = std::make_shared<talcite::ui::Column>();

    look->children().add({std::make_shared<talcite::ui::Span>("Colors follow the terminal."),
                          std::make_shared<talcite::ui::Tick>("Unicode frames"),
                          std::make_shared<talcite::ui::Tick>("Pictures in sixels")});

    const auto keys = std::make_shared<talcite::ui::Column>();
    const auto reset = std::make_shared<talcite::ui::Button>("Reset keys");

    reset->keys().add(U'R');

    keys->children().add({std::make_shared<talcite::ui::Span>("F1 help, F10 quit, Ctrl+PgDn next page."),
                          std::make_shared<talcite::ui::Span>("R on this page alone resets the keys."), reset});

    const auto tabs = std::make_shared<talcite::ui::Tabs>();

    tabs->children().add({tabbed("General", U'G', general), tabbed("Look", U'L', look), tabbed("Keys", U'K', keys)});

    const auto ok = std::make_shared<talcite::ui::Button>("OK");

    ok->primary(true);

    const auto column = std::make_shared<talcite::ui::Column>();

    column->children().add({tabs, ok});
    frame->children().add(column);
    made->children().add(frame);

    const std::weak_ptr<talcite::ui::Dialog> weak = made;

    made->heard.push_back(reset->on<talcite::ui::PickEvent>([told](talcite::ui::PickEvent&) {
        told->text("keys reset");

        return true;
    }));

    made->heard.push_back(ok->on<talcite::ui::PickEvent>([weak, told, name](talcite::ui::PickEvent&) {
        told->text("name: " + name->text());

        if (const auto one = weak.lock()) one->close();

        return true;
    }));

    return made;
}

}

// A screen with a key bar: F5 opens the window with the tabs.
int main() {
    demo::Hooks heard;

    talcite::Term term;

    Sheet sheet{term};

    const auto dialogs = std::make_shared<talcite::ui::Modals>();
    const auto bar = std::make_shared<talcite::ui::KeyBar>();
    const auto holder = std::make_shared<talcite::ui::FocusHolder>();
    const auto frame = std::make_shared<talcite::ui::Frame>("talcite — tabs");
    const auto said = std::make_shared<talcite::ui::Span>("F5 opens a window with three tabs");

    frame->children().add(said);
    holder->children().add(frame);

    // The holder covers the whole content of the window, whatever the frame wants: a click anywhere
    // in it takes the focus, and without focus the key bar would not get the keys.
    const auto room = std::make_shared<talcite::ui::Box>();

    room->children().add(holder);
    bar->content(room);
    dialogs->content(bar);

    (void)demo::doing(heard, bar->add(talcite::ui::Code::f5, "Settings"),
                      [dialogs, said] { dialogs->open(demo::closing(settings(said))); });

    (void)demo::doing(heard, bar->add(talcite::ui::Code::f10, "Quit"), [&term] { term.stop(); });

    sheet.children().add(dialogs);

    holder->focus();

    term.show(sheet);
    term.title("talcite — tabs");

    term.run();

    return 0;
}
