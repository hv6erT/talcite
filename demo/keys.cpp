#include <deque>
#include <string>

#include "talcite/encoding.hpp"

#include "talcite/term.hpp"

namespace {

// The printed name of a key that is not a letter; "?" for a code without a name.
std::string named(talcite::ui::Code code) {
    switch (code) {
        case talcite::ui::Code::escape: return "Esc";
        case talcite::ui::Code::enter: return "Enter";
        case talcite::ui::Code::tab: return "Tab";
        case talcite::ui::Code::backspace: return "Backspace";
        case talcite::ui::Code::insert: return "Insert";
        case talcite::ui::Code::remove: return "Delete";
        case talcite::ui::Code::up: return "Up";
        case talcite::ui::Code::down: return "Down";
        case talcite::ui::Code::left: return "Left";
        case talcite::ui::Code::right: return "Right";
        case talcite::ui::Code::home: return "Home";
        case talcite::ui::Code::end: return "End";
        case talcite::ui::Code::pageup: return "PgUp";
        case talcite::ui::Code::pagedown: return "PgDn";
        case talcite::ui::Code::f1: return "F1";
        case talcite::ui::Code::f2: return "F2";
        case talcite::ui::Code::f3: return "F3";
        case talcite::ui::Code::f4: return "F4";
        case talcite::ui::Code::f5: return "F5";
        case talcite::ui::Code::f6: return "F6";
        case talcite::ui::Code::f7: return "F7";
        case talcite::ui::Code::f8: return "F8";
        case talcite::ui::Code::f9: return "F9";
        case talcite::ui::Code::f10: return "F10";
        case talcite::ui::Code::f11: return "F11";
        case talcite::ui::Code::f12: return "F12";
        case talcite::ui::Code::none: break;
    }

    return "?";
}

// The modifiers held with a key, as a prefix such as "Ctrl+Alt+"; empty when none is held.
std::string marked(talcite::ui::Mods mods) {
    std::string made;

    if (mods.ctrl) made += "Ctrl+";
    if (mods.alt) made += "Alt+";
    if (mods.shift) made += "Shift+";

    return made;
}

// The whole application is one root: the reactions append a line to the list, and `paint` draws the
// list. Nothing here writes to the terminal directly; `Term` composes the frame and sends only the
// difference.
class Sheet : public talcite::ui::Root {
public:
    explicit Sheet(talcite::Term& term) : term_(term) {}

protected:
    // F10 stops the terminal; any other key is logged with its modifiers, as a quoted letter or by
    // its name.
    bool handle(talcite::ui::KeyEvent& what) override {
        if (what.key() == talcite::ui::Code::f10) {
            term_.stop();

            return true;
        }

        std::string said = marked(what.mods());

        if (what.key().letter() != 0) said += "'" + talcite::utf8::encode(what.key().letter()) + "'";
        else said += named(what.key().code());

        shown("key", said);

        return true;
    }

    // Logs a mouse press, release or move, with the cell where it happened.
    bool handle(talcite::ui::MouseEvent& what) override {
        const std::string where =
            std::to_string(what.where().x) + ", " + std::to_string(what.where().y);

        const std::string doing = what.press() == talcite::ui::Press::down   ? "press"
                                  : what.press() == talcite::ui::Press::up   ? "release"
                                                                          : "move";

        shown("mouse", doing + " at " + where);

        return true;
    }

    // Logs a click with its cell.
    bool handle(talcite::ui::ClickEvent& what) override {
        shown("click", std::to_string(what.where().x) + ", " + std::to_string(what.where().y));

        return true;
    }

    // Logs a double click with its cell.
    bool handle(talcite::ui::DoubleClickEvent& what) override {
        shown("double click",
              std::to_string(what.where().x) + ", " + std::to_string(what.where().y));

        return true;
    }

    // Logs which way the wheel turned.
    bool handle(talcite::ui::WheelEvent& what) override {
        shown("wheel", what.steps() > 0 ? "up" : "down");

        return true;
    }

    // Logs a paste with the size of the pasted text in bytes.
    bool handle(talcite::ui::PasteEvent& what) override {
        shown("paste", std::to_string(what.text().size()) + " bytes");

        return true;
    }

    // Logs the new size of the terminal.
    bool handle(talcite::ui::SizeEvent& what) override {
        shown("size", std::to_string(what.wide()) + " by " + std::to_string(what.high()));

        return true;
    }

    // A bold heading, then the logged lines below it, as many as fit.
    talcite::View paint(talcite::ui::Size given, talcite::ui::Area seen) override {
        talcite::View made{seen.size};

        talcite::Style head;
        head.bold = true;

        made.put({2 - seen.at.x, 0 - seen.at.y}, "Press keys. F10 quits.", head);

        int y = 2;

        for (const std::string& one : said_) {
            if (y >= given.high) break;

            made.put({2 - seen.at.x, y - seen.at.y}, one, talcite::Style{});

            ++y;
        }

        return made;
    }

private:
    // Appends one line to the log and drops the oldest lines that no longer fit on the screen.
    void shown(const std::string& what, const std::string& said) {
        said_.push_back(what + ": " + said);

        const std::size_t room =
            static_cast<std::size_t>(term_.size().high > 3 ? term_.size().high - 3 : 1);

        while (said_.size() > room) said_.pop_front();
    }

    talcite::Term& term_;
    std::deque<std::string> said_;
};

}

// Shows every key, mouse, paste and size event the terminal delivers, one per line.
int main() {
    talcite::Term term;

    Sheet sheet{term};

    term.show(sheet);
    term.title("talcite — keys");

    term.run();

    return 0;
}
