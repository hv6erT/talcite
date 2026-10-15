#include <memory>
#include <string>

#include "talcite/button.hpp"
#include "talcite/column.hpp"
#include "talcite/dialog.hpp"
#include "talcite/modals.hpp"
#include "talcite/field.hpp"
#include "talcite/frame.hpp"
#include "talcite/legenditem.hpp"
#include "talcite/legend.hpp"
#include "talcite/list.hpp"
#include "talcite/listitem.hpp"
#include "talcite/row.hpp"
#include "talcite/span.hpp"
#include "talcite/term.hpp"

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

// A made-up file name for a row: every seventh is a directory, every fifth a name too long to fit.
std::string named(int which) {
    const std::string number = std::to_string(which);

    if (which % 7 == 0) return "directory-" + number;

    if (which % 5 == 0) return "a-very-long-file-name-that-will-not-fit-" + number + ".txt";

    return "file-" + number + ".txt";
}

// A window with a list: choosing one entry. Tab goes field, list, buttons, and the arrows inside
// the list do not take Tab away: for the tour the list is one stop, even though it has a hundred
// willing rows inside.
std::shared_ptr<demo::Kept<talcite::ui::Dialog>> asking(const std::shared_ptr<talcite::ui::Span>& told) {
    const auto made = std::make_shared<demo::Kept<talcite::ui::Dialog>>();

    const auto frame = std::make_shared<talcite::ui::Frame>(
        "Choose a file", talcite::ui::FrameShape{talcite::ui::Border::twin});

    const auto column = std::make_shared<talcite::ui::Column>();

    column->children().add(std::make_shared<talcite::ui::Span>("Name:"));

    const auto field = std::make_shared<talcite::ui::Field>(34);

    column->children().add(field);
    column->children().add(std::make_shared<talcite::ui::Span>(" "));

    const auto list = std::make_shared<talcite::ui::List>(34, 8, talcite::ui::ScrollShape{true});

    for (int which = 0; which < 40; ++which)
        list->children().add(std::make_shared<talcite::ui::ListItem>(named(which)));

    // The selected row is written into the field. This is a `ScrollEvent`, not a `PickEvent`: it
    // speaks up on every change of row, not only on confirmation. The event does not carry the row;
    // the listener takes it from `current()`, since the list holds it anyway.
    const std::weak_ptr<talcite::ui::List> weakly = list;

    made->heard.push_back(list->on<talcite::ui::ScrollEvent>([field, weakly](talcite::ui::ScrollEvent&) {
        if (const auto one = weakly.lock())
            if (const talcite::ui::ListItem* item = one->current()) field->text(item->text());

        return false;
    }));

    column->children().add(list);
    column->children().add(std::make_shared<talcite::ui::Span>(" "));

    const auto yes = std::make_shared<talcite::ui::Button>("Open");
    const auto no = std::make_shared<talcite::ui::Button>("Stop");

    yes->keys().add(U'O');
    no->keys().add(U'P');
    yes->primary(true);

    const auto row = std::make_shared<talcite::ui::Row>();

    row->children().add(yes);
    row->children().add(no);
    column->children().add(row);

    frame->children().add(column);
    made->children().add(frame);

    const std::weak_ptr<talcite::ui::Dialog> weak = made;

    const auto shutting = [weak, told, field](const std::string& said) {
        return [weak, told, field, said] {
            told->text(said + field->text());

            if (const auto one = weak.lock()) one->close();
        };
    };

    made->heard.push_back(yes->on<talcite::ui::PickEvent>([what = shutting("opened: ")](talcite::ui::PickEvent&) {
        what();

        return true;
    }));
    made->heard.push_back(no->on<talcite::ui::PickEvent>(
        [what = shutting("canceled, the field holds: ")](talcite::ui::PickEvent&) {
            what();

            return true;
        }));

    // Enter and a double click on a row do the same as the primary button.
    for (const std::shared_ptr<talcite::ui::Node>& one : list->children())
        if (const auto item = std::dynamic_pointer_cast<talcite::ui::ListItem>(one))
            made->heard.push_back(item->on<talcite::ui::PickEvent>(
                [what = shutting("opened: ")](talcite::ui::PickEvent&) {
                    what();

                    return true;
                }));

    return made;
}

}

// A full screen: the list fills the frame, the bar on the right shows where we are, and the line
// above it says what is current. F3 opens a window in which the same kind of list stands next to a
// field and buttons.
int main() {
    demo::Hooks heard;

    talcite::Term term;

    Sheet sheet{term};

    const auto dialogs = std::make_shared<talcite::ui::Modals>();
    const auto legend = std::make_shared<talcite::ui::Legend>();

    const auto frame = std::make_shared<talcite::ui::Frame>("talcite — list");
    const auto column = std::make_shared<talcite::ui::Column>();
    const auto told = std::make_shared<talcite::ui::Span>("arrows, PgUp, PgDn, Home, End, wheel");

    const auto list = std::make_shared<talcite::ui::List>(talcite::ui::plenty, talcite::ui::plenty,
                                                       talcite::ui::ScrollShape{true});

    for (int which = 0; which < 200; ++which)
        list->children().add(std::make_shared<talcite::ui::ListItem>(named(which)));

    // Every tenth entry is disabled: moving through the list has to skip them, not stop on them.
    for (int which = 0; which < 200; which += 10)
        if (const auto one = std::dynamic_pointer_cast<talcite::ui::ListItem>(
                list->children()[static_cast<std::size_t>(which)]))
            one->enabled(false);

    const std::weak_ptr<talcite::ui::List> weakly = list;

    heard.push_back(list->on<talcite::ui::ScrollEvent>([told, weakly](talcite::ui::ScrollEvent&) {
        if (const auto one = weakly.lock())
            if (const talcite::ui::ListItem* item = one->current())
                told->text("row " + std::to_string(one->which().value_or(0)) + ": " + item->text());

        return false;
    }));

    for (const std::shared_ptr<talcite::ui::Node>& one : list->children())
        if (const auto item = std::dynamic_pointer_cast<talcite::ui::ListItem>(one)) {
            const std::weak_ptr<talcite::ui::ListItem> weak = item;

            heard.push_back(item->on<talcite::ui::PickEvent>([told, weak](talcite::ui::PickEvent&) {
                if (const auto which = weak.lock()) told->text("opened: " + which->text());

                return true;
            }));
        }

    column->children().add(told);
    column->children().add(list);
    frame->children().add(column);

    legend->content(frame);
    dialogs->content(legend);

    (void)demo::doing(heard, legend->add(talcite::ui::Code::f3, "Choose"),
                      [dialogs, told] { dialogs->open(demo::closing(asking(told))); });

    (void)demo::doing(heard, legend->add(talcite::ui::Code::f10, "Quit"), [&term] { term.stop(); });

    sheet.children().add(dialogs);

    list->focus();

    term.show(sheet);
    term.title("talcite — list");

    term.run();

    return 0;
}
