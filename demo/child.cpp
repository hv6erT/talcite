#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "talcite/child.hpp"
#include "talcite/column.hpp"
#include "talcite/field.hpp"
#include "talcite/frame.hpp"
#include "talcite/keybaritem.hpp"
#include "talcite/keybar.hpp"
#include "talcite/promise.hpp"
#include "talcite/skin.hpp"
#include "talcite/span.hpp"
#include "talcite/term.hpp"
#include "talcite/viewer.hpp"

#include "doing.hpp"

namespace {

class Sheet : public talcite::ui::Root {
public:
    explicit Sheet(talcite::Term& term) : term_(term) {}

protected:
    // F10 stops the terminal; every other key is taken, so nothing leaks past the root.
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

// The demo's skin: light text on blue, with cyan input fields and a black-and-cyan key bar.
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
        {"talcite.page", painted(light, blue)},
        {"talcite.field", painted(black, cyan)},
        {"talcite.field.focus", painted(black, light)},
        {"talcite.legend.key", painted(light, black)},
        {"talcite.legend.text", painted(black, cyan)},
    }};
}

// The shell is what we run, not something talcite assumes: the demo writes `sh -c` because it wants
// to pass a whole sentence, and only the demo knows what the shell is called on its system. The
// library gets a plain recipe with a program and its arguments.
talcite::Command spoken(const std::string& said) {
    talcite::Command made;

#ifdef _WIN32

    made.program = talcite::fs::path{"cmd.exe"};
    made.arguments.emplace_back("/c");

#else

    made.program = talcite::fs::path{"/bin/sh"};
    made.arguments.emplace_back("-c");

#endif

    made.arguments.push_back(said);

    return made;
}

// The standard output with the standard error appended to it, for showing both in one viewer.
talcite::Bytes joined(talcite::Bytes read, const talcite::Bytes& errors) {
    read.insert(read.end(), errors.begin(), errors.end());

    return read;
}

}

// Running other programs, in two ways. Enter collects the output for the viewer: the command runs
// alongside, and the window keeps working normally in the meantime. F4 hands the whole terminal to
// the program and waits: this is how Midnight Commander opens an editor or less, and it is the only
// way programs that draw on the screen themselves can work.
int main() {
    demo::Hooks heard;

    talcite::Term term;

    Sheet sheet{term};

    const auto bar = std::make_shared<talcite::ui::KeyBar>();
    const auto frame = std::make_shared<talcite::ui::Frame>("output");
    const auto column = std::make_shared<talcite::ui::Column>();

    const auto told = std::make_shared<talcite::ui::Span>("Enter — collect output   F4 — hand over the screen");
    const auto viewer = std::make_shared<talcite::ui::Viewer>();
    const auto field = std::make_shared<talcite::ui::Field>(talcite::ui::plenty, "talcite --version");

    frame->children().add(viewer);

    column->children().add(told);
    column->children().add(frame);
    column->children().add(field);

    bar->content(column);

    const auto showing = [told, viewer](const talcite::Output& made) {
        viewer->hold(joined(made.out, made.err));

        told->text("exit code: " + std::to_string(made.exit.code));
    };

    // Collecting runs alongside. `here()` moves the continuation onto the drawing loop, so the node
    // tree is touched on the same thread that draws it, and the window responds in the meantime.
    //
    // We keep the promise: work goes on only while someone wants it, so a dropped promise would
    // stop the program and its callbacks would never run.
    const auto kept = std::make_shared<std::optional<talcite::Promise<void>>>();

    const auto gathering = [field, told, showing, kept] {
        told->text("running…");

        kept->emplace(talcite::Child::run(spoken(field->text()))
                          .here()
                          .then([showing](const talcite::Output& made) { showing(made); })
                          .recover([told](const std::exception& why) {
                              told->text(std::string{"failed: "} + why.what());
                          }));
    };

    heard.push_back(field->on<talcite::ui::KeyEvent>([gathering](talcite::ui::KeyEvent& what) {
        if (what.key() != talcite::ui::Code::enter) return false;

        gathering();

        return true;
    }));

    (void)demo::doing(heard, bar->add(talcite::ui::Code::f3, "Collect"), gathering);

    // Handing over the screen is an ordinary call that takes time: the drawing loop stands still,
    // because the terminal now belongs to someone else. The pool threads (the clock, the network,
    // file reading) keep running.
    (void)demo::doing(heard, bar->add(talcite::ui::Code::f4, "Hand over"), [&term, field, told] {
        const talcite::Exit made = term.execute(spoken(field->text()));

        told->text("returned with code " + std::to_string(made.code));
    });

    (void)demo::doing(heard, bar->add(talcite::ui::Code::f10, "Quit"), [&term] { term.stop(); });

    sheet.children().add(bar);
    sheet.skin(coloured());

    field->focus();

    term.show(sheet);
    term.title("talcite — other programs");

    term.run();

    return 0;
}
