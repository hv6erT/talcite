#include <memory>
#include <string>

#include "talcite/column.hpp"
#include "talcite/frame.hpp"
#include "talcite/span.hpp"
#include "talcite/term.hpp"
#include "talcite/scroll.hpp"

namespace {

// Two hundred rows in a scroller, in a frame. The wheel scrolls, F10 quits. Only what is visible
// gets drawn; the rest does not cost a single cell.
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

}

// Two hundred numbered rows in a framed vertical scroller.
int main() {
    talcite::Term term;

    Sheet sheet{term};

    const auto scroll = std::make_shared<talcite::ui::Scroll>(talcite::ui::Axis::vertical, talcite::ui::ScrollShape{true});
    const auto column = std::make_shared<talcite::ui::Column>();

    for (int which = 0; which < 200; ++which)
        column->children().add(std::make_shared<talcite::ui::Span>("row number " + std::to_string(which)));

    scroll->children().add(column);

    const auto frame = std::make_shared<talcite::ui::Frame>("talcite — scrolling");

    frame->footer("F10 quits");
    frame->children().add(scroll);

    sheet.children().add(frame);

    term.show(sheet);
    term.title("talcite — scrolling");

    term.run();

    return 0;
}
