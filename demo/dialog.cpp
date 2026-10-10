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
#include "talcite/line.hpp"
#include "talcite/menu.hpp"
#include "talcite/menuitem.hpp"
#include "talcite/menuseparator.hpp"
#include "talcite/menubar.hpp"
#include "talcite/pane.hpp"
#include "talcite/row.hpp"
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

// A dialog's box is an ordinary frame with content. The dialog does three things to it: centers it,
// casts a shadow and swallows everything that did not land in it. The double line is as in Midnight
// Commander.
std::shared_ptr<talcite::ui::Dialog> asking(const std::string& title, const std::string& said,
                                         const std::shared_ptr<talcite::ui::Span>& told) {
    const auto made = std::make_shared<talcite::ui::Dialog>();

    const auto frame =
        std::make_shared<talcite::ui::Frame>(title, talcite::ui::FrameShape{talcite::ui::Border::twin});

    const auto column = std::make_shared<talcite::ui::Column>();

    column->children().add(std::make_shared<talcite::ui::Span>(said));
    column->children().add(std::make_shared<talcite::ui::Span>(" "));

    // Tab moves between the buttons, Enter presses the one with focus, a hot key works from
    // anywhere in the window, and Esc closes it. Marking the primary button draws the bracket with
    // arrows and nothing more: pressing belongs to the focus.
    const auto yes = std::make_shared<talcite::ui::Button>("Yes");
    const auto no = std::make_shared<talcite::ui::Button>("No");
    const auto stop = std::make_shared<talcite::ui::Button>("Stop");

    // The label is plain text and the hot keys are given separately. A letter that occurs in the
    // label is underlined by itself.
    yes->keys().add(U'Y');
    no->keys().add(U'N');
    stop->keys().add(std::vector<talcite::ui::Key>{U'P', talcite::ui::Code::f10});

    yes->primary(true);

    const auto row = std::make_shared<talcite::ui::Row>();

    row->children().add(yes);
    row->children().add(no);
    row->children().add(stop);

    column->children().add(row);

    frame->children().add(column);
    made->children().add(frame);

    const std::weak_ptr<talcite::ui::Dialog> weak = made;

    const auto answered = [weak, told](std::string what) {
        return [weak, told, what = std::move(what)] {
            told->text("answer: " + what);

            if (const auto one = weak.lock()) one->close();
        };
    };

    (void)yes->on<talcite::ui::PickEvent>([what = answered("Yes")](talcite::ui::PickEvent&) {
        what();

        return true;
    });
    (void)no->on<talcite::ui::PickEvent>([what = answered("No")](talcite::ui::PickEvent&) {
        what();

        return true;
    });
    (void)stop->on<talcite::ui::PickEvent>([what = answered("Stop")](talcite::ui::PickEvent&) {
        what();

        return true;
    });

    return made;
}

// A window with a field: asking for a name. Enter confirms, Esc closes, and Tab completes the
// suggestion; when there is nothing to complete, it moves on to the button.
std::shared_ptr<talcite::ui::Dialog> named(const std::shared_ptr<talcite::ui::Span>& told) {
    const auto made = std::make_shared<talcite::ui::Dialog>();

    const auto frame = std::make_shared<talcite::ui::Frame>(
        "Rename", talcite::ui::FrameShape{talcite::ui::Border::twin});

    const auto column = std::make_shared<talcite::ui::Column>();

    column->children().add(std::make_shared<talcite::ui::Span>("New name (try: ca, do, fi):"));

    const auto field = std::make_shared<talcite::ui::Field>(30);

    // The caller computes the suggestion. Here it comes from a handful of names; in a real program
    // it would come from a directory, but from a ready list, not a trip to the disk, because this
    // is called on every change of the content.
    field->guess([](const std::string& what) {
        static const char* these[] = {"catalog", "cable", "documents", "files.txt"};

        if (what.empty()) return std::string{};

        for (const char* one : these) {
            const std::string which{one};

            if (which.size() > what.size() && which.compare(0, what.size(), what) == 0)
                return which;
        }

        return std::string{};
    });

    column->children().add(field);

    // The separator reaches the frame: the frame itself paints the joints `├` and `┤`, because a
    // child may not paint outside its own rectangle.
    column->children().add(std::make_shared<talcite::ui::Line>("What next"));

    const auto yes = std::make_shared<talcite::ui::Button>("Rename");
    const auto no = std::make_shared<talcite::ui::Button>("Stop");

    yes->keys().add(U'R');
    no->keys().add(U'P');
    yes->primary(true);

    const auto row = std::make_shared<talcite::ui::Row>();

    row->children().add(yes);
    row->children().add(no);
    column->children().add(row);

    frame->children().add(column);
    made->children().add(frame);

    const std::weak_ptr<talcite::ui::Dialog> weak = made;

    (void)yes->on<talcite::ui::PickEvent>([weak, told, field](talcite::ui::PickEvent&) {
        told->text("new name: " + field->text());

        if (const auto one = weak.lock()) one->close();

        return true;
    });

    (void)no->on<talcite::ui::PickEvent>([weak, told](talcite::ui::PickEvent&) {
        told->text("rename canceled");

        if (const auto one = weak.lock()) one->close();

        return true;
    });

    return made;
}

}

// A full screen, in the order that follows from what lies on top of what: the dialog covers the
// legend, the legend covers the menu bar, and the menu bar covers the content.
int main() {
    talcite::Term term;

    Sheet sheet{term};

    const auto dialogs = std::make_shared<talcite::ui::Modals>();
    const auto legend = std::make_shared<talcite::ui::Legend>();
    const auto menus = std::make_shared<talcite::ui::Menubar>();

    // The application gives the key that opens the menus; the library assigns none by itself.
    menus->keys().add(talcite::ui::Key{talcite::ui::Code::f9});


    const auto pane = std::make_shared<talcite::ui::Pane>();
    const auto frame = std::make_shared<talcite::ui::Frame>("talcite — dialog box");
    const auto scroll = std::make_shared<talcite::ui::Scroll>(talcite::ui::Axis::vertical, talcite::ui::ScrollShape{true});
    const auto column = std::make_shared<talcite::ui::Column>();
    const auto said =
        std::make_shared<talcite::ui::Span>("F5 asks yes/no, F6 asks for a name with a suggestion; Tab, "
                                         "Enter, a letter and Esc work in the window");

    column->children().add(said);

    for (int which = 0; which < 60; ++which)
        column->children().add(std::make_shared<talcite::ui::Span>("row number " + std::to_string(which)));

    scroll->children().add(column);
    frame->children().add(scroll);
    pane->children().add(frame);
    menus->content(pane);
    legend->content(menus);
    dialogs->content(legend);

    const auto file = menus->add("File");

    file->children().add(demo::doing(std::make_shared<talcite::ui::MenuItem>("Copy", "F5"), [dialogs, said] {
        dialogs->open(demo::closing(asking("Copy", "Copy the selected files?", said)));
    }));
    file->children().add(std::make_shared<talcite::ui::MenuSeparator>());
    file->children().add(demo::doing(std::make_shared<talcite::ui::MenuItem>("Exit", "F10"),
                          [&term] { term.stop(); }));

    (void)menus->add("View");

    (void)demo::doing(legend->add(talcite::ui::Code::f1, "Help"), [dialogs, said] {
        dialogs->open(
            asking("Help", "The dialog is modal: nothing underneath responds.", said));
    });

    (void)demo::doing(legend->add(talcite::ui::Code::f5, "Copy"), [dialogs, said] {
        dialogs->open(demo::closing(asking("Copy", "Copy the selected files?", said)));
    });

    (void)demo::doing(legend->add(talcite::ui::Code::f6, "Rename"),
                      [dialogs, said] { dialogs->open(demo::closing(named(said))); });

    (void)demo::doing(legend->add(talcite::ui::Code::f9, "PullDn"), [menus] {
        if (!menus->menus().empty()) menus->menus().front()->focus();
    });

    (void)demo::doing(legend->add(talcite::ui::Code::f10, "Quit"), [&term] { term.stop(); });

    sheet.children().add(dialogs);

    pane->focus();

    term.show(sheet);
    term.title("talcite — dialog box");

    term.run();

    return 0;
}
