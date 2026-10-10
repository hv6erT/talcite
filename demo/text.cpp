#include <memory>
#include <string>

#include "talcite/column.hpp"
#include "talcite/span.hpp"
#include "talcite/term.hpp"

namespace {

// Two texts: one wrapped, the other not. Change the window width and watch which one breaks and
// which one disappears under the edge. F10 quits.
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

// A wrapped and an unwrapped text in one column, to compare as the window width changes.
int main() {
    talcite::Term term;

    Sheet sheet{term};

    // The root takes one child, because it is not a layout. The column is the layout.
    const auto column = std::make_shared<talcite::ui::Column>();

    sheet.children().add(column);

    column->children().add(std::make_shared<talcite::ui::Span>(
        "This text wraps, so in a narrow window it spreads over several lines, "
        "and in a wide one it fits on one.",
        talcite::ui::SpanShape{true}));

    column->children().add(std::make_shared<talcite::ui::Span>(
        "This text does not wrap and simply disappears under the edge when there is no room left."));

    column->children().add(std::make_shared<talcite::ui::Span>("F10 quits."));

    term.show(sheet);
    term.title("talcite — text");

    term.run();

    return 0;
}
