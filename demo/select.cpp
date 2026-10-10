#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "talcite/column.hpp"
#include "talcite/dialog.hpp"
#include "talcite/encoding.hpp"
#include "talcite/frame.hpp"
#include "talcite/legenditem.hpp"
#include "talcite/legend.hpp"
#include "talcite/row.hpp"
#include "talcite/select.hpp"
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

// The Midnight Commander palette. The select gets its own family of roles; the widget code knows
// nothing about it, it asks for roles and gets what this palette says.
talcite::ui::Skin coloured() {
    const talcite::Color blue = tinted(0, 0, 160);
    const talcite::Color light = tinted(200, 200, 200);
    const talcite::Color white = tinted(255, 255, 255);
    const talcite::Color cyan = tinted(0, 160, 160);
    const talcite::Color black = tinted(0, 0, 0);
    const talcite::Color yellow = tinted(255, 230, 80);

    return talcite::ui::Skin{{
        {"talcite.normal", painted(light, blue)},
        {"talcite.frame", painted(light, blue)},
        {"talcite.title", painted(yellow, blue)},

        {"talcite.select", painted(black, cyan)},
        {"talcite.select.focus", painted(black, light)},
        {"talcite.select.disabled", painted(blue, cyan)},

        {"talcite.list", painted(black, cyan)},
        {"talcite.list.focus", painted(black, light)},

        {"talcite.legend.key", painted(light, black)},
        {"talcite.legend.text", painted(black, cyan)},

        {"talcite.message", painted(white, blue)},
        {"talcite.message.title", painted(yellow, blue)},
        {"talcite.message.text", painted(white, blue)},
    }};
}

// The encodings offered by the first select, a mix of Western, Central European, Cyrillic and East
// Asian ones.
std::vector<std::string> encodings() {
    return {"UTF-8",   "ISO-8859-2",   "CP1250",   "CP852",  "KOI8-R",
            "Big5",    "Shift_JIS",    "EUC-KR",   "IBM866", "MacRoman"};
}

// A setting row: the label on the left, the select on the right. A row gives each child as much as
// the child wants, so the labels do not line up by themselves; padding to a fixed width does that.
//
// We pad in cells, not bytes: a label with non-ASCII letters has more bytes than cells, so counted
// in bytes the padding would come out too short.
std::shared_ptr<talcite::ui::Row> setting(const std::string& said,
                                        const std::shared_ptr<talcite::ui::Select>& one) {
    const auto row = std::make_shared<talcite::ui::Row>();

    const int wide = talcite::measured(said).wide;
    const auto label =
        std::make_shared<talcite::ui::Span>(said + std::string(std::max(16 - wide, 1), ' '));

    row->children().add(label);
    row->children().add(one);

    return row;
}

}

// A drop-down list. Closed, it is one row with the value and an arrow; open, it shows the rest in a
// box below itself, or above itself when there is no room below. The box lies on top of everything,
// because it is a veil in the same window as the dialogs.
int main() {
    talcite::Term term;

    Sheet sheet{term};

    const auto legend = std::make_shared<talcite::ui::Legend>();
    const auto modals = std::make_shared<talcite::ui::Modals>();

    const auto frame = std::make_shared<talcite::ui::Frame>("Settings");
    const auto column = std::make_shared<talcite::ui::Column>();

    const auto told = std::make_shared<talcite::ui::Span>(
        "Enter opens the list, the arrows change the value without opening it.");

    const auto coding = std::make_shared<talcite::ui::Select>(encodings());
    const auto ending = std::make_shared<talcite::ui::Select>(
        std::vector<std::string>{"LF (Unix)", "CRLF (Windows)", "CR (classic Mac)"});
    const auto sorting = std::make_shared<talcite::ui::Select>(
        std::vector<std::string>{"name", "extension", "size", "modification time", "unsorted"});

    const auto said = std::make_shared<talcite::ui::Span>("");

    column->children().add(told);
    column->children().add(std::make_shared<talcite::ui::Span>(""));
    column->children().add(setting("Encoding", coding));
    column->children().add(setting("Line endings", ending));
    column->children().add(setting("Sorting", sorting));
    column->children().add(std::make_shared<talcite::ui::Span>(""));
    column->children().add(said);

    frame->children().add(column);
    modals->content(frame);
    legend->content(modals);

    // Every choice announces the same event, whether it was made with the mouse, with Enter or with
    // an arrow on the closed row.
    const auto watching = [said](const std::shared_ptr<talcite::ui::Select>& one,
                                 const std::string& named) {
        (void)one->on<talcite::ui::PickEvent>([said, one, named](talcite::ui::PickEvent&) {
            said->text(named + ": " + one->value().value_or(std::string{}));

            return true;
        });
    };

    watching(coding, "Encoding");
    watching(ending, "Line endings");
    watching(sorting, "Sorting");

    (void)demo::doing(legend->add(talcite::ui::Code::f2, "Set"), [coding, said] {
        // A value set by the program is silent: filling a window with initial values is not
        // choosing. The index and the text are the same selected option; the index is easier to
        // step on from, and the text is what is shown.
        const std::size_t next =
            (coding->which().value_or(0) + 1) % std::max(coding->options().many(), std::size_t{1});

        coding->which(next);

        said->text("set by the program (no event): " +
                   coding->value().value_or(std::string{}));
    });

    (void)demo::doing(legend->add(talcite::ui::Code::f10, "Quit"), [&term] { term.stop(); });

    sheet.children().add(legend);
    sheet.skin(coloured());

    coding->focus();

    term.show(sheet);
    term.title("talcite — drop-down list");

    term.run();

    return 0;
}
