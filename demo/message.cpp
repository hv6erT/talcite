#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "talcite/button.hpp"
#include "talcite/column.hpp"
#include "talcite/modals.hpp"
#include "talcite/frame.hpp"
#include "talcite/keybaritem.hpp"
#include "talcite/keybar.hpp"
#include "talcite/list.hpp"
#include "talcite/listitem.hpp"
#include "talcite/menu.hpp"
#include "talcite/menuitem.hpp"
#include "talcite/menuseparator.hpp"
#include "talcite/menubar.hpp"
#include "talcite/message.hpp"
#include "talcite/skin.hpp"
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

// A true-color value from its three components.
talcite::Color tinted(std::uint8_t red, std::uint8_t green, std::uint8_t blue) {
    return talcite::Color{false, red, green, blue};
}

// A style with the given foreground and background and nothing else set.
talcite::Style painted(talcite::Color foreground, talcite::Color background) {
    talcite::Style made{};

    made.foreground = foreground;
    made.background = background;

    return made;
}

// A palette that recreates Midnight Commander: navy panels, a teal menu bar, a red error box. The
// widget code knows nothing about it; it asks for roles and gets what this palette says.
talcite::ui::Skin coloured() {
    const talcite::Color blue = tinted(0, 0, 160);
    const talcite::Color light = tinted(200, 200, 200);
    const talcite::Color white = tinted(255, 255, 255);
    const talcite::Color cyan = tinted(0, 160, 160);
    const talcite::Color black = tinted(0, 0, 0);
    const talcite::Color yellow = tinted(255, 230, 80);
    const talcite::Color red = tinted(190, 0, 0);

    return talcite::ui::Skin{{
        {"talcite.normal", painted(light, blue)},
        {"talcite.frame", painted(light, blue)},
        {"talcite.title", painted(yellow, blue)},

        {"talcite.list", painted(light, blue)},
        {"talcite.list.focus", painted(black, cyan)},

        {"talcite.menubar", painted(black, cyan)},
        {"talcite.menubar.focus", painted(black, light)},
        {"talcite.menu", painted(black, cyan)},
        {"talcite.menu.focus", painted(black, light)},
        {"talcite.menu.frame", painted(black, cyan)},

        {"talcite.legend.key", painted(light, black)},
        {"talcite.legend.text", painted(black, cyan)},

        // An ordinary message: the same window as a panel, only with its own family of roles.
        {"talcite.message", painted(light, blue)},
        {"talcite.message.title", painted(yellow, blue)},
        {"talcite.message.text", painted(white, blue)},
        {"talcite.message.box", painted(light, blue)},
        {"talcite.message.button", painted(black, cyan)},
        {"talcite.message.button.focus", painted(black, light)},
        {"talcite.message.button.key", painted(yellow, cyan)},

        // The error box. Ink equal to the paper in the role `talcite.error` hides the line of the
        // outer frame, and what remains is a column of breathing space around the content. There is
        // no margin in the code; there is a frame edge that cannot be seen.
        {"talcite.error", painted(red, red)},
        {"talcite.error.title", painted(yellow, red)},
        {"talcite.error.text", painted(white, red)},
        {"talcite.error.box", painted(white, red)},
        {"talcite.error.button", painted(white, red)},
        {"talcite.error.button.focus", painted(white, red)},
        {"talcite.error.button.key", painted(yellow, red)},
    }};
}

}

// Notifications as in Midnight Commander. One class, four shapes: a message with one button, a
// question with several answers, an error box, and a box without buttons shown while work goes on.
// The number of answers and the role stem make the difference, nothing else.
int main() {
    demo::Hooks heard;

    talcite::Term term;

    Sheet sheet{term};

    const auto bar = std::make_shared<talcite::ui::KeyBar>();
    const auto menus = std::make_shared<talcite::ui::Menubar>();

    // The application gives the key that opens the menus; the library assigns none by itself.
    menus->keys().add(talcite::ui::Key{talcite::ui::Code::f9});

    const auto dialogs = std::make_shared<talcite::ui::Modals>();

    const auto frame = std::make_shared<talcite::ui::Frame>("/etc");
    const auto column = std::make_shared<talcite::ui::Column>();

    const auto told = std::make_shared<talcite::ui::Span>("F2 message  F3 question  F4 error  "
                                                       "F5 waiting  F6 skin");

    const auto list = std::make_shared<talcite::ui::List>(talcite::ui::plenty, talcite::ui::plenty,
                                                       talcite::ui::ScrollShape{true});

    for (const char* one :
         {"..", "GNUstep", "ImageMagick-7", "ModemManager", "NetworkManager", "ODBCDataSources",
          "PackageKit", "UPower", "X11", "apt", "avahi", "bash_completion.d", "binfmt.d",
          "bluetooth", "byobu", "ca-certificates"})
        list->children().add(std::make_shared<talcite::ui::ListItem>(one));

    column->children().add(told);
    column->children().add(list);

    frame->children().add(column);
    dialogs->content(frame);
    menus->content(dialogs);
    bar->content(menus);

    const auto file = menus->add("File");

    file->children().add(std::make_shared<talcite::ui::MenuItem>("Copy", "F5"));
    file->children().add(std::make_shared<talcite::ui::MenuSeparator>());
    file->children().add(demo::doing(heard, std::make_shared<talcite::ui::MenuItem>("Exit", "F10"),
                          [&term] { term.stop(); }));

    (void)menus->add("View");

    // The action of an answer sits on its button, exactly as in a key bar item and in a menu item.
    // The message does not number the answers and does not hand them back to the caller; after a
    // press it simply closes.
    //
    // "Closed without an answer" is a separate notice, because Esc has no button on which an action
    // could hang. A flag tells one from the other, because an answer also ends the window.
    const auto answering = [told](const std::shared_ptr<demo::Kept<talcite::ui::Message>>& one,
                                  std::vector<std::pair<std::string, char32_t>> these) {
        const auto answered = std::make_shared<bool>(false);

        // The caller does the closing, because it is part of the answer's action: a message does
        // not leave the screen by itself. The handler holds a weak pointer to the window, since a
        // strong one would make a cycle: the window holds the button, and the button holds this
        // handler.
        const std::weak_ptr<talcite::ui::Message> weak = one;

        for (const std::pair<std::string, char32_t>& what : these) {
            const std::shared_ptr<talcite::ui::Button> made = one->buttons().add(what.first);

            if (what.second != 0) made->keys().add(what.second);

            (void)demo::doing(one->heard, made, [told, answered, weak, name = what.first] {
                *answered = true;

                told->text("answer: " + name);

                if (const auto kept = weak.lock()) kept->close();
            });
        }

        one->heard.push_back(one->on<talcite::ui::CloseEvent>([told, answered](talcite::ui::CloseEvent&) {
            if (!*answered) told->text("answer: Esc");

            return true;
        }));
    };

    (void)demo::doing(heard, bar->add(talcite::ui::Code::f2, "Msg"), [dialogs, answering] {
        const auto one =
            std::make_shared<demo::Kept<talcite::ui::Message>>("Notice", "All changes have been saved.");

        answering(one, {{"OK", U'O'}});

        dialogs->open(demo::closing(one));
    });

    (void)demo::doing(heard, bar->add(talcite::ui::Code::f3, "Ask"), [dialogs, answering] {
        const auto one = std::make_shared<demo::Kept<talcite::ui::Message>>(
            "File exists",
            "The file “ca-certificates"
            "” already exists.\nWhat should be done with it?");

        answering(one, {{"Overwrite", U'O'}, {"Skip", U'P'}, {"Abort", U'a'}});

        one->buttons()[0]->primary(true);

        dialogs->open(demo::closing(one));
    });

    // An error is the same widget with a different stem. No separate class, no flag in the code:
    // one call, and the palette says the rest.
    (void)demo::doing(heard, bar->add(talcite::ui::Code::f4, "Error"), [dialogs, answering] {
        const auto one =
            std::make_shared<demo::Kept<talcite::ui::Message>>("Error", "Cannot operate on “..”!");

        one->role("talcite.error");

        answering(one, {{"OK", U'O'}});

        dialogs->open(demo::closing(one));
    });

    // Without answers: a box that the program closes, not the user. Esc closes it too, because it
    // is still a dialog; talcite has no means to make a dialog impossible to close, and will not
    // have one.
    (void)demo::doing(heard, bar->add(talcite::ui::Code::f5, "Wait"), [dialogs] {
        dialogs->open(demo::closing(std::make_shared<demo::Kept<talcite::ui::Message>>("Wait", "Scanning the directory…")));
    });

    const auto plain = std::make_shared<bool>(false);

    (void)demo::doing(heard, bar->add(talcite::ui::Code::f6, "Skin"), [&sheet, plain] {
        *plain = !*plain;

        sheet.skin(*plain ? talcite::ui::Skin{} : coloured());
    });

    (void)demo::doing(heard, bar->add(talcite::ui::Code::f9, "PullDn"), [menus] {
        if (!menus->menus().empty()) menus->menus().front()->focus();
    });

    (void)demo::doing(heard, bar->add(talcite::ui::Code::f10, "Quit"), [&term] { term.stop(); });

    sheet.children().add(bar);
    sheet.skin(coloured());

    list->focus();

    term.show(sheet);
    term.title("talcite — notifications");

    term.run();

    return 0;
}
