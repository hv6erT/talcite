#include <memory>
#include <string>

#include "talcite/column.hpp"
#include "talcite/frame.hpp"
#include "talcite/span.hpp"
#include "talcite/term.hpp"
#include "talcite/scroll.hpp"

namespace {

// Dwieście wierszy w przewijaku, w ramce. Kółko przewija, F10 kończy. Rysuje się tylko to, co
// widać — reszta nie kosztuje ani jednej komórki.
class Sheet : public talcite::ui::Root {
public:
    explicit Sheet(talcite::Term& term) : term_(term) {}

protected:
    bool handle(talcite::ui::KeyEvent& what) override {
        if (what.key() == talcite::ui::Code::f10) term_.stop();

        return true;
    }

private:
    talcite::Term& term_;
};

}

int main() {
    talcite::Term term;

    Sheet sheet{term};

    const auto scroll = std::make_shared<talcite::ui::Scroll>(talcite::ui::Axis::vertical, talcite::ui::ScrollShape{true});
    const auto column = std::make_shared<talcite::ui::Column>();

    for (int which = 0; which < 200; ++which)
        column->children().add(std::make_shared<talcite::ui::Span>("wiersz numer " + std::to_string(which)));

    scroll->children().add(column);

    const auto frame = std::make_shared<talcite::ui::Frame>("talcite — przewijanie");

    frame->footer("F10 kończy");
    frame->children().add(scroll);

    sheet.children().add(frame);

    term.show(sheet);
    term.title("talcite — przewijanie");

    term.run();

    return 0;
}
