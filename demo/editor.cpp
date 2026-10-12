#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "talcite/button.hpp"
#include "talcite/bytes.hpp"
#include "talcite/column.hpp"
#include "talcite/comb.hpp"
#include "talcite/dialog.hpp"
#include "talcite/modals.hpp"
#include "talcite/editor.hpp"
#include "talcite/field.hpp"
#include "talcite/frame.hpp"
#include "talcite/fs.hpp"
#include "talcite/legenditem.hpp"
#include "talcite/legend.hpp"
#include "talcite/line.hpp"
#include "talcite/promise.hpp"
#include "talcite/row.hpp"
#include "talcite/skin.hpp"
#include "talcite/span.hpp"
#include "talcite/term.hpp"
#include "talcite/tick.hpp"
#include "talcite/view.hpp"

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

    // Lets the status bar recompute its padding when the terminal changes size. The event goes on
    // to others.
    bool handle(talcite::ui::SizeEvent&) override {
        if (sized) sized();

        return false;
    }

private:
    talcite::Term& term_;
};

// Built-in text. The editor has something to show even when no path is given, and the text holds
// everything that tends to break: tabs, an empty line, non-ASCII letters and a line longer than the
// window.
constexpr const char* sample =
    "talcite — text editor (Zażółć gęślą jaźń)\n"
    "\n"
    "The document lives in a piece table, so the file is never read in whole. Typing a character\n"
    "splits one piece into three, deleting splits and drops, and the original stays where it\n"
    "was — behind the source. Memory grows with the number of changes, not with the file size.\n"
    "\n"
    "Keys\n"
    "\tF2\tsave\n"
    "\tF3\tsearch\n"
    "\tF4\treplace\n"
    "\tF5\tline wrapping\n"
    "\tF7\tundo\n"
    "\tF8\tredo\n"
    "\tF10\texit\n"
    "\n"
    "Arrows move by characters and lines, Home and End along the line, PageUp and PageDown by screen,\n"
    "and Ctrl with an arrow by words. With Shift, each of them extends the selection.\n"
    "\n"
    "Mouse: a click places the cursor — also past a tab and in the wrapped half of a line, because\n"
    "a column is not a byte and a row is not a line. A double click takes a word, and dragging selects;\n"
    "at the bottom edge the view follows the cursor as fast as you move the mouse.\n"
    "\n"
    "Clipboard\n"
    "\n"
    "\tCtrl+C\tCtrl+Ins\tcopy\n"
    "\tCtrl+X\tShift+Del\tcut\n"
    "\tCtrl+V\tShift+Ins\tpaste\n"
    "\n"
    "The clipboard is the system's, not ours: on Windows it goes to the system clipboard, and on Linux\n"
    "and over ssh we ask the terminal to put it into the clipboard of the machine you are sitting at.\n"
    "The other way, the terminal pastes by itself with its own shortcut — and that works regardless.\n"
    "\n"
    "Undo\n"
    "\n"
    "A typed word is undone as a whole, not letter by letter: space, tab and enter close the group,\n"
    "and moving the cursor ends it. Replacing all occurrences comes back in one step, even if there\n"
    "were a hundred changes. An asterisk on the top bar means the document has unsaved changes — and\n"
    "it goes out by itself when you undo everything back.\n"
    "\n"
    "Search\n"
    "\n"
    "The comb searches within one line, and the editor walks the document — because only it knows\n"
    "where a line begins. So a pattern does not cross the end of a line. In the search window you can\n"
    "turn on regular expressions, letter case and whole words.\n"
    "\n"
    "Saving\n"
    "\n"
    "The library does not touch files: the application provides both the source and the sink. This\n"
    "demo writes alongside, to a file ending in .new, and says so on the bar — because writing over the\n"
    "file the source reads from would destroy the document.\n";

// The bytes of a string, as the editor takes its content.
talcite::Bytes lump(const std::string& what) {
    talcite::Bytes made;

    made.reserve(what.size());

    for (const char sign : what) made.push_back(static_cast<std::byte>(sign));

    return made;
}

// A reversed style, for the status bar.
talcite::Style turned() {
    talcite::Style made{};

    made.reverse = true;

    return made;
}

// The search window: a pattern and three switches. A bad pattern is not an exception: the comb says
// it is not ready, and that goes to the bar.
//
// Focus lands in the field by itself: the window puts it on the first node that wants it, and the
// field stands here before the buttons.
std::shared_ptr<talcite::ui::Dialog>
hunting(const std::string& title, bool swapping,
        std::function<void(std::string, std::string, talcite::Sift)> after) {
    const auto made = std::make_shared<talcite::ui::Dialog>();

    const auto frame =
        std::make_shared<talcite::ui::Frame>(title, talcite::ui::FrameShape{talcite::ui::Border::twin});

    const auto column = std::make_shared<talcite::ui::Column>();

    column->children().add(std::make_shared<talcite::ui::Span>("Search for:"));

    const auto what = std::make_shared<talcite::ui::Field>(40);

    column->children().add(what);

    const auto with = std::make_shared<talcite::ui::Field>(40);

    if (swapping) {
        column->children().add(std::make_shared<talcite::ui::Span>("Replace with:"));
        column->children().add(with);
    }

    column->children().add(std::make_shared<talcite::ui::Line>("How"));

    // Three independent switches. Each one keeps its own state, so there is nothing to track from
    // outside; we read them only when the window is confirmed. A hot letter works from anywhere in
    // the window and is underlined in the label.
    const auto regex = std::make_shared<talcite::ui::Tick>("regular expression");
    const auto cased = std::make_shared<talcite::ui::Tick>("letter case");
    const auto whole = std::make_shared<talcite::ui::Tick>("complete words");

    regex->keys().add(U'g');
    cased->keys().add(U'l');
    whole->keys().add(U'c');

    column->children().add(regex);
    column->children().add(cased);
    column->children().add(whole);

    column->children().add(std::make_shared<talcite::ui::Line>("What next"));

    const auto yes = std::make_shared<talcite::ui::Button>(swapping ? "Replace" : "Search");
    const auto no = std::make_shared<talcite::ui::Button>("Stop");

    yes->keys().add(swapping ? U'R' : U'S');
    no->keys().add(U'P');
    yes->primary(true);

    const auto row = std::make_shared<talcite::ui::Row>();

    row->children().add(yes);
    row->children().add(no);
    column->children().add(row);

    frame->children().add(column);
    made->children().add(frame);

    const std::weak_ptr<talcite::ui::Dialog> weak = made;

    (void)yes->on<talcite::ui::PickEvent>(
        [weak, what, with, regex, cased, whole, after = std::move(after)](talcite::ui::PickEvent&) {
            if (after)
                after(what->text(), with->text(),
                     talcite::Sift{regex->ticked(), cased->ticked(), whole->ticked()});

            if (const auto one = weak.lock()) one->close();

            return true;
        });

    (void)no->on<talcite::ui::PickEvent>([weak](talcite::ui::PickEvent&) {
        if (const auto one = weak.lock()) one->close();

        return true;
    });

    return made;
}

}

// An editor over the file named on the command line, or over the built-in text when there is none.
// Saving writes next to the file, to the same name with `.new` appended.
int main(int argc, char** argv) {
    talcite::Term term;

    Sheet sheet{term};

    sheet.skin(talcite::ui::Skin{{{"demo.bar", turned()}}});

    const auto dialogs = std::make_shared<talcite::ui::Modals>();
    const auto legend = std::make_shared<talcite::ui::Legend>();
    const auto column = std::make_shared<talcite::ui::Column>();

    const auto bar = std::make_shared<talcite::ui::Span>(std::string{});
    const auto shown = std::make_shared<talcite::ui::Editor>(talcite::ui::ScrollShape{true});

    // The application binds the clipboard keys, because the editor assigns no key to an action: both
    // sets call the same methods, Ctrl+C, Ctrl+X and Ctrl+V as elsewhere and Ctrl+Ins, Shift+Del and
    // Shift+Ins as in Midnight Commander. In raw mode Ctrl+C is not an interrupt but an ordinary key,
    // and a widget that swallowed it would take away the application's chance to use it for quitting.
    {
        const std::weak_ptr<talcite::ui::Editor> weak = shown;

        (void)shown->on<talcite::ui::KeyEvent>([weak](talcite::ui::KeyEvent& what) {
            const talcite::ui::Mods mods = what.mods();

            if (mods.alt) return false;

            const auto mine = weak.lock();

            if (!mine) return false;

            if (what.key().code() == talcite::ui::Code::insert) {
                if (mods.ctrl && !mods.shift) return mine->copy();
                if (mods.shift && !mods.ctrl) return mine->paste();

                return false;
            }

            if (what.key().code() == talcite::ui::Code::remove) return mods.shift && !mods.ctrl && mine->cut();

            if (!mods.ctrl) return false;

            switch (what.key().letter()) {
                case U'c': return mine->copy();
                case U'x': return mine->cut();
                case U'v': return mine->paste();
                default: return false;
            }
        });
    }

    bar->role("demo.bar");
    bar->wrapped(false);

    column->children().add(bar);
    column->children().add(shown);

    legend->content(column);
    dialogs->content(legend);

    sheet.children().add(dialogs);

    const auto source = std::make_shared<std::string>("built-in text");
    const auto told = std::make_shared<std::string>();

    // On the left the source with an asterisk, on the right the line and the column. The column is
    // counted in cells, not bytes, so after a tab it jumps by eight, exactly as it looks.
    const auto naming = [&term, bar, shown, source, told] {
        std::string left = *source;

        if (shown->dirty()) left += " *";

        if (!told->empty()) left += "   " + *told;

        const std::string right = "line " + std::to_string(shown->line() + 1) + " column " +
                                  std::to_string(shown->column() + 1) + "  UTF-8";

        const int room =
            term.size().wide - talcite::measured(left).wide - talcite::measured(right).wide;

        bar->text(left + std::string(room > 1 ? static_cast<std::size_t>(room) : 1, ' ') + right);
    };

    sheet.sized = naming;

    // The listener hangs on the column, not on the bar: the event bubbles up from the editor, and
    // the bar is its sibling. The observer returns false so as not to hide the event from anyone
    // above.
    (void)column->on<talcite::ui::ScrollEvent>([naming](talcite::ui::ScrollEvent&) {
        naming();

        return false;
    });

    const auto saying = [told, naming](std::string what) {
        *told = std::move(what);

        naming();
    };

    const auto where = std::make_shared<std::string>();

    // Saving goes through the pool: `fs::open` opens the file, `File::write` writes the pieces, and
    // the editor clears its journal only once the whole save is fulfilled. The promise has to be
    // kept, or it would die with this closure.
    const auto saving = std::make_shared<std::optional<talcite::Promise<void>>>();

    (void)demo::doing(legend->add(talcite::ui::Code::f2, "Save"), [shown, where, saying, saving] {
        if (where->empty()) {
            saying("nowhere to save");

            return;
        }

        if (shown->busy()) {
            saying("saving in progress");

            return;
        }

        saying("saving…");

        const std::filesystem::path onto = talcite::fs::from_string(*where);

        saving->emplace(
            talcite::fs::open(onto, talcite::fs::Mode::truncate)
                .here()
                .then([shown](talcite::fs::File file) {
                    const auto at = std::make_shared<std::uintmax_t>(0);

                    return shown->save([file, at](talcite::Bytes got) mutable {
                        // An empty piece is the end: there is nothing to write, only the file to
                        // close.
                        if (got.empty()) {
                            file.close();

                            return talcite::ready();
                        }

                        const std::uintmax_t from = *at;

                        *at += got.size();

                        return file.write(from, std::move(got)).then([](std::size_t) {});
                    });
                })
                .here()
                .then([where, saying] { saying("saved to " + *where); })
                .recover([saying](const std::exception& why) {
                    saying(std::string{"save failed: "} + why.what());
                }));
    });

    (void)demo::doing(legend->add(talcite::ui::Code::f3, "Search"), [dialogs, shown, saying] {
        dialogs->open(demo::closing(hunting(
            "Search", false,
            [shown, saying](std::string what, std::string, talcite::Sift how) {
                const talcite::Comb comb{std::move(what), how};

                if (!comb.ready()) {
                    saying(comb.wrong());

                    return;
                }

                shown->focus();

                if (shown->find(comb, shown->cursor(), false).has_value())
                    saying({});
                else
                    saying("not found");
            })));
    });

    (void)demo::doing(legend->add(talcite::ui::Code::f4, "Replace"), [dialogs, shown, saying] {
        dialogs->open(demo::closing(hunting(
            "Replace", true,
            [shown, saying](std::string what, std::string with, talcite::Sift how) {
                const talcite::Comb comb{std::move(what), how};

                if (!comb.ready()) {
                    saying(comb.wrong());

                    return;
                }

                shown->focus();

                const std::size_t much = shown->replace_all(comb, with);

                saying(much > 0 ? "replaced: " + std::to_string(much)
                                : std::string{"not found"});
            })));
    });

    (void)demo::doing(legend->add(talcite::ui::Code::f5, "Wrap"), [shown, naming] {
        shown->wrapped(!shown->wrapped());

        naming();
    });

    (void)demo::doing(legend->add(talcite::ui::Code::f7, "Undo"), [shown, saying] {
        shown->undo();

        saying({});
    });

    (void)demo::doing(legend->add(talcite::ui::Code::f8, "Redo"), [shown, saying] {
        shown->redo();

        saying({});
    });

    (void)demo::doing(legend->add(talcite::ui::Code::f10, "Quit"),
                      [&term] { term.stop(); });

    // The file is read in whole, because the editor is resident, but the pool reads it (`fs::read`)
    // and the editor gets the bytes on a turn of the loop; until then it is empty.
    std::optional<talcite::Promise<void>> opening;

    if (argc > 1) {
        const std::filesystem::path from = talcite::fs::from_string(argv[1]);

        *where = std::string{argv[1]} + ".new";

        opening.emplace(talcite::fs::read(from)
                            .here()
                            .then([shown, source, from](talcite::Bytes got) {
                                *source = talcite::fs::to_string(from);

                                shown->hold(std::move(got));
                            })
                            .recover([shown, source, from](const std::exception&) {
                                *source = "could not open: " + talcite::fs::to_string(from);

                                shown->hold(lump(sample));
                            }));
    } else {
        shown->hold(lump(sample));

        *where = "editor.new";
    }

    term.show(sheet);
    term.title("talcite — text editor");

    shown->focus();

    naming();

    term.run();

    return 0;
}
