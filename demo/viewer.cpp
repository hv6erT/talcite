#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "talcite/bytes.hpp"
#include "talcite/column.hpp"
#include "talcite/fs.hpp"
#include "talcite/legenditem.hpp"
#include "talcite/legend.hpp"
#include "talcite/mark.hpp"
#include "talcite/promise.hpp"
#include "talcite/skin.hpp"
#include "talcite/span.hpp"
#include "talcite/term.hpp"
#include "talcite/view.hpp"
#include "talcite/viewer.hpp"

#include "doing.hpp"

namespace {

class Sheet : public talcite::ui::Root {
public:
    explicit Sheet(talcite::Term& term) : term_(term) {}

    std::function<void()> sized;

protected:
    // F10 stops the terminal; every other key is reported as handled.
    bool handle(talcite::ui::KeyEvent& what) override {
        if (what.key() == talcite::ui::Code::f10) term_.stop();

        return true;
    }

    // The top bar is padded to the width of the window, so after a resize it has to be put together
    // again. The event passes through the root anyway, so there is nothing else to add.
    bool handle(talcite::ui::SizeEvent&) override {
        if (sized) sized();

        return false;
    }

private:
    talcite::Term& term_;
};

// Text built into the program. The viewer has something to show even when no path is given, and the
// text holds everything that tends to break: a paragraph longer than the window, tabs, an empty
// line and one very long line.
constexpr const char* sample =
    "talcite — text viewer\n"
    "\n"
    "This text is built into the demo. Running it with an argument opens the given file, and then\n"
    "the viewer takes it as a stream: it reads only the pieces that are needed at the moment, so\n"
    "opening a two-gigabyte log costs as much as opening this note.\n"
    "\n"
    "Keys\n"
    "\tF2\tline wrapping\n"
    "\tF3\tnext mark\n"
    "\tF4\tposition in lines or in bytes\n"
    "\tF5\tprevious mark\n"
    "\tF10\texit\n"
    "\n"
    "Arrows, PageUp, PageDown, Home and End scroll. So does the mouse wheel.\n"
    "\n"
    "What the top bar shows\n"
    "\n"
    "On the left the source, on the right the position. With the built-in text the number of\n"
    "lines is known at once, because the document lies in memory and counting it takes microseconds.\n"
    "With a file read as a stream the number of lines is unknown, and the bar shows a question mark "
    "until\n"
    "you reach the end, if only with the End key. Then the question mark disappears and never comes\n"
    "back. This is exactly what the plus means in Midnight Commander.\n"
    "\n"
    "The byte position is always known, because both ways go through bytes. If the viewer got a\n"
    "line source (one that hands over ready lines and knows nothing about bytes), the byte position\n"
    "would be empty, and the bar would then show only lines.\n"
    "\n"
    "Wrapping\n"
    "\n"
    "Below is one line longer than any window. With wrapping on, it spreads "
    "over\n"
    "several screen rows, and the down arrow goes through them in turn instead of jumping straight\n"
    "to the next line of the document. With wrapping off, the same line is cut at the edge, and\n"
    "the line number stays the same — switching never moves you to another place in the\n"
    "document.\n"
    "\n"
    "One two three four five six seven eight nine ten eleven twelve thirteen "
    "fourteen fifteen sixteen seventeen eighteen nineteen twenty "
    "twenty-one twenty-two twenty-three twenty-four twenty-five "
    "twenty-six twenty-seven twenty-eight twenty-nine thirty.\n"
    "\n"
    "Tabs\n"
    "\n"
    "column\tsecond\tthird\n"
    "a\tbb\tccc\n"
    "longer\tfield\tbeside\n"
    "\n"
    "A tab expands to eight columns only when drawn. In the content it stays a single byte "
    "—\n"
    "otherwise the byte position would stop matching the file on the disk.\n"
    "\n"
    "Characters wider than a cell\n"
    "\n"
    "你好世界 — each of these characters takes two columns, so cutting a line counts columns, "
    "not\n"
    "characters or bytes. In a narrow window you can see that none of them sticks out past the edge.\n"
    "\n"
    "The end.\n";

// The bytes of a string, as the viewer takes its content.
talcite::Bytes lump(const std::string& what) {
    talcite::Bytes made;

    made.reserve(what.size());

    for (const char sign : what) made.push_back(static_cast<std::byte>(sign));

    return made;
}

// The application does the searching, not the viewer. The widget gets ready ranges and only paints
// them, just as Decoration.mark in CodeMirror knows nothing about where it came from. The line
// numbering has to agree with `Spool`: a line ending at the end of the text does not make an empty
// line after itself.
std::vector<talcite::ui::Mark> hunted(const std::string& what, const std::string& word) {
    std::vector<talcite::ui::Mark> made;

    std::size_t line = 0;

    for (std::size_t at = 0;;) {
        const std::size_t end = what.find('\n', at);
        const std::size_t upto = end == std::string::npos ? what.size() : end;

        const std::string one = what.substr(at, upto - at);

        for (std::size_t got = one.find(word); got != std::string::npos;
             got = one.find(word, got + 1))
            made.push_back(talcite::ui::Mark{line, got, got + word.size(), "talcite.viewer.mark"});

        ++line;

        if (end == std::string::npos) break;

        at = end + 1;

        if (at == what.size()) break;
    }

    return made;
}

// A reversed style, for the status bar.
talcite::Style turned() {
    talcite::Style made{};

    made.reverse = true;

    return made;
}

}

// A viewer over the file named on the command line, read as a stream, or over the built-in text
// with every occurrence of one word marked.
int main(int argc, char** argv) {
    talcite::Term term;

    Sheet sheet{term};

    // One role of our own on top of the built-in skin: the `Skin` constructor adds to the built-in
    // values, so the rest of the look stays untouched. The prefix is `demo.`, because the library's
    // names begin with `talcite.` and nobody else's may cover them.
    sheet.skin(talcite::ui::Skin{{{"demo.bar", turned()}}});

    const auto legend = std::make_shared<talcite::ui::Legend>();
    const auto column = std::make_shared<talcite::ui::Column>();

    const auto bar = std::make_shared<talcite::ui::Span>(std::string{});
    const auto shown = std::make_shared<talcite::ui::Viewer>(talcite::ui::ScrollShape{true});

    bar->role("demo.bar");
    bar->wrapped(false);

    column->children().add(bar);
    column->children().add(shown);

    legend->content(column);

    sheet.children().add(legend);

    const auto source = std::make_shared<std::string>("built-in text");
    const auto counted = std::make_shared<bool>(false);

    // The position in lines or in bytes: the viewer gives both, and the choice belongs to the
    // application. A question mark means "not known yet", not "none".
    const auto placed = [shown, counted] {
        if (*counted) {
            if (!shown->at().has_value()) return std::string{"bytes unknown"};

            return std::to_string(*shown->at()) + "/" + std::to_string(*shown->whole());
        }

        if (!shown->first().has_value()) return std::string{"empty"};

        std::string made = "line " + std::to_string(*shown->first() + 1) + "/";

        made += shown->lines().has_value() ? std::to_string(*shown->lines()) : std::string{"?"};

        return made;
    };

    const auto naming = [&term, bar, source, placed] {
        const std::string left = *source;
        const std::string right = placed() + "  UTF-8";

        const int room =
            term.size().wide - talcite::measured(left).wide - talcite::measured(right).wide;

        bar->text(left + std::string(room > 1 ? static_cast<std::size_t>(room) : 1, ' ') + right);
    };

    sheet.sized = naming;

    // The listener hangs on the column, not on the bar. The event bubbles up from the viewer, and
    // the bar is its sibling, so on the bar it would hear nothing. The observer returns false so as
    // not to hide the event from anyone above.
    (void)column->on<talcite::ui::ScrollEvent>([naming](talcite::ui::ScrollEvent&) {
        naming();

        return false;
    });

    (void)demo::doing(legend->add(talcite::ui::Code::f2, "Wrap"), [shown, naming] {
        shown->wrapped(!shown->wrapped());

        naming();
    });

    (void)demo::doing(legend->add(talcite::ui::Code::f3, "Next"),
                      [shown] { (void)shown->step(1); });

    (void)demo::doing(legend->add(talcite::ui::Code::f5, "Prev"),
                      [shown] { (void)shown->step(-1); });

    (void)demo::doing(legend->add(talcite::ui::Code::f4, "Pos"), [counted, naming] {
        *counted = !*counted;

        naming();
    });

    (void)demo::doing(legend->add(talcite::ui::Code::f10, "Quit"),
                      [&term] { term.stop(); });

    // The file is read as a stream and asynchronously. `fs::open` and `File::read` go through the
    // pool, and the viewer gets a `Feed`, a source that returns a promise: no window waits for the
    // disk on the main thread. Opening does not wait either; until it arrives the viewer is empty.
    // The file handle lives in the closure, so nobody closes it before the viewer is done with it.
    std::optional<talcite::Promise<void>> opening;

    if (argc > 1) {
        const std::filesystem::path where = talcite::fs::from_string(argv[1]);

        opening.emplace(
            talcite::fs::open(where)
                .then([](talcite::fs::File file) {
                    return file.size().then([file](std::uintmax_t whole) {
                        return std::pair{file, static_cast<std::size_t>(whole)};
                    });
                })
                .here()
                .then([shown, source, where](std::pair<talcite::fs::File, std::size_t> got) {
                    *source = talcite::fs::to_string(where);

                    shown->hold(
                        [file = got.first](std::size_t at, std::size_t most) { return file.read(at, most); },
                        got.second);
                })
                .recover([shown, source, where](const std::exception&) {
                    *source = "could not open: " + talcite::fs::to_string(where);

                    shown->hold(lump(sample));
                }));
    } else {
        const std::string said = sample;

        shown->hold(lump(said));

        // We mark every occurrence of one word, so that there is something to paint and to jump
        // between. We do not do it for a file from the argument: the demo does not hold it in
        // memory, and walking two gigabytes to highlight something would be exactly the search that
        // does not exist yet.
        shown->marks().all(hunted(said, "line"));
    }

    term.show(sheet);

    // Without focus, keys end their journey at the root and the legend never sees them. The viewer
    // takes them itself, so a `Pane` is not needed here.
    shown->focus();

    naming();

    term.run();

    return 0;
}
