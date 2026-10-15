#include <cstdint>
#include <memory>
#include <string>

#include "talcite/column.hpp"
#include "talcite/frame.hpp"
#include "talcite/historyfield.hpp"
#include "talcite/legenditem.hpp"
#include "talcite/legend.hpp"
#include "talcite/list.hpp"
#include "talcite/listitem.hpp"
#include "talcite/row.hpp"
#include "talcite/skin.hpp"
#include "talcite/span.hpp"
#include "talcite/term.hpp"
#include "talcite/modals.hpp"

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

// The demo's skin: light text on blue, cyan fields and a cyan cursor in the list.
talcite::ui::Skin coloured() {
    const talcite::Color blue = tinted(0, 0, 160);
    const talcite::Color light = tinted(200, 200, 200);
    const talcite::Color cyan = tinted(0, 160, 160);
    const talcite::Color black = tinted(0, 0, 0);
    const talcite::Color yellow = tinted(255, 230, 80);

    return talcite::ui::Skin{{
        {"talcite.normal", painted(light, blue)},
        {"talcite.frame", painted(light, blue)},
        {"talcite.title", painted(yellow, blue)},

        {"talcite.field", painted(black, cyan)},
        {"talcite.field.focus", painted(black, light)},

        {"talcite.list", painted(light, blue)},
        {"talcite.list.focus", painted(black, cyan)},

        {"talcite.legend.key", painted(light, black)},
        {"talcite.legend.text", painted(black, cyan)},
    }};
}

}

// A command line with history, as under the panels of Midnight Commander. The arrows walk through
// past entries in place, and Alt+H unfolds them as a list below the field.
//
// Note who adds the entries: the application, not the widget. The field does not know when a
// command has been accepted, since Enter in a window presses the primary button rather than
// confirming the line. Only whoever builds the window knows that, and it is the one that calls
// `add`.
int main() {
    demo::Hooks heard;

    talcite::Term term;

    Sheet sheet{term};

    const auto legend = std::make_shared<talcite::ui::Legend>();
    const auto modals = std::make_shared<talcite::ui::Modals>();

    const auto column = std::make_shared<talcite::ui::Column>();
    const auto frame = std::make_shared<talcite::ui::Frame>("Done");
    const auto done = std::make_shared<talcite::ui::List>(talcite::ui::plenty, talcite::ui::plenty);

    const auto told = std::make_shared<talcite::ui::Span>(
        "Enter runs, the arrows walk the history, Alt+H unfolds it as a list.");

    const auto line = std::make_shared<talcite::ui::Row>();
    const auto sign = std::make_shared<talcite::ui::Span>("Command: ");
    const auto said = std::make_shared<talcite::ui::HistoryField>(talcite::ui::plenty);

    // The application gives the key that unfolds the list, because the library assigns none by
    // itself, and it lies where the keys of every other widget lie, in `keys()`. The modifier is
    // part of the key, so "h" without Alt is still an ordinary letter to type.
    {
        talcite::ui::Mods alt;

        alt.alt = true;

        said->keys().add(talcite::ui::Key{U'h', alt});
    }

    // History loaded from an "old settings file". It is added entry by entry, so it goes through
    // the removal of repeats and through the limit just like any other.
    for (const char* one :
         {"make -j8", "cmake --build out", "git status", "grep -rn TODO src", "ls -la /etc"})
        said->options().insert(0, one);

    said->text("");

    line->children().add(sign);
    line->children().add(said);

    frame->children().add(done);

    // The command line stands above the list, not below it, and this is not a matter of taste. The
    // column gives its children, in turn, as much as they want, so the one that asks for everything
    // takes the rest: a frame with the list placed before the command line would push the line off
    // the screen.
    //
    // In Midnight Commander the line stands below the panels, because the key bar reserves the last
    // row for itself with a separate window. talcite does not have a pin-to-bottom layout yet.
    column->children().add(told);
    column->children().add(line);
    column->children().add(frame);

    modals->content(column);
    legend->content(modals);

    // Enter does not belong to the field: the field lets it through, so we catch it here, and this
    // is the place where a command counts as given.
    heard.push_back(said->on<talcite::ui::KeyEvent>([said, done](talcite::ui::KeyEvent& what) {
        if (what.key() != talcite::ui::Code::enter) return false;

        if (said->text().empty()) return true;

        done->children().add(std::make_shared<talcite::ui::ListItem>(said->text()));

        said->options().insert(0, said->text());
        said->text("");

        return true;
    }));

    // Taking an entry from the past announces one event, whether it was done with an arrow or from
    // the unfolded list.
    heard.push_back(said->on<talcite::ui::PickEvent>([said, told](talcite::ui::PickEvent&) {
        told->text("taken from history: " + said->text());

        return true;
    }));

    (void)demo::doing(heard, legend->add(talcite::ui::Code::f3, "History"),
                      [said] { said->open(); });

    (void)demo::doing(heard, legend->add(talcite::ui::Code::f8, "Forget"), [said, told] {
        said->clear();

        told->text("history cleared");
    });

    (void)demo::doing(heard, legend->add(talcite::ui::Code::f10, "Quit"), [&term] { term.stop(); });

    sheet.children().add(legend);
    sheet.skin(coloured());

    said->focus();

    term.show(sheet);
    term.title("talcite — command history");

    term.run();

    return 0;
}
