#include <deque>
#include <string>

#include "talcite/encoding.hpp"

#include "talcite/term.hpp"

namespace {

std::string named(talcite::ui::Code code) {
    switch (code) {
        case talcite::ui::Code::escape: return "Esc";
        case talcite::ui::Code::enter: return "Enter";
        case talcite::ui::Code::tab: return "Tab";
        case talcite::ui::Code::backspace: return "Backspace";
        case talcite::ui::Code::insert: return "Insert";
        case talcite::ui::Code::remove: return "Delete";
        case talcite::ui::Code::up: return "W górę";
        case talcite::ui::Code::down: return "W dół";
        case talcite::ui::Code::left: return "W lewo";
        case talcite::ui::Code::right: return "W prawo";
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

std::string marked(talcite::ui::Mods mods) {
    std::string made;

    if (mods.ctrl) made += "Ctrl+";
    if (mods.alt) made += "Alt+";
    if (mods.shift) made += "Shift+";

    return made;
}

// Cała aplikacja to jeden korzeń: reakcje dopisują wiersz do listy, a paint rysuje listę.
// Nikt tu nie pisze na terminal wprost — Term składa klatkę i wysyła samą różnicę.
class Sheet : public talcite::ui::Root {
public:
    explicit Sheet(talcite::Term& term) : term_(term) {}

protected:
    bool handle(talcite::ui::KeyEvent& what) override {
        if (what.key() == talcite::ui::Code::f10) {
            term_.stop();

            return true;
        }

        std::string said = marked(what.mods());

        if (what.key().letter() != 0) said += "'" + talcite::utf8::encode(what.key().letter()) + "'";
        else said += named(what.key().code());

        shown("klawisz", said);

        return true;
    }

    bool handle(talcite::ui::MouseEvent& what) override {
        const std::string where =
            std::to_string(what.where().x) + ", " + std::to_string(what.where().y);

        const std::string doing = what.press() == talcite::ui::Press::down   ? "wciśnięcie"
                                  : what.press() == talcite::ui::Press::up   ? "puszczenie"
                                                                          : "ruch";

        shown("mysz", doing + " w " + where);

        return true;
    }

    bool handle(talcite::ui::ClickEvent& what) override {
        shown("kliknięcie", std::to_string(what.where().x) + ", " + std::to_string(what.where().y));

        return true;
    }

    bool handle(talcite::ui::DoubleClickEvent& what) override {
        shown("podwójne kliknięcie",
              std::to_string(what.where().x) + ", " + std::to_string(what.where().y));

        return true;
    }

    bool handle(talcite::ui::WheelEvent& what) override {
        shown("kółko", what.steps() > 0 ? "w górę" : "w dół");

        return true;
    }

    bool handle(talcite::ui::PasteEvent& what) override {
        shown("wklejenie", std::to_string(what.text().size()) + " bajtów");

        return true;
    }

    bool handle(talcite::ui::SizeEvent& what) override {
        shown("rozmiar", std::to_string(what.wide()) + " na " + std::to_string(what.high()));

        return true;
    }

    talcite::View paint(talcite::ui::Size given, talcite::ui::Area seen) override {
        talcite::View made{seen.size};

        talcite::Style head;
        head.bold = true;

        made.put({2 - seen.at.x, 0 - seen.at.y}, "Naciskaj klawisze. F10 kończy.", head);

        int y = 2;

        for (const std::string& one : said_) {
            if (y >= given.high) break;

            made.put({2 - seen.at.x, y - seen.at.y}, one, talcite::Style{});

            ++y;
        }

        return made;
    }

private:
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

int main() {
    talcite::Term term;

    Sheet sheet{term};

    term.show(sheet);
    term.title("talcite — klawisze");

    term.run();

    return 0;
}
