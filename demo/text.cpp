#include <memory>
#include <string>

#include "talcite/column.hpp"
#include "talcite/span.hpp"
#include "talcite/term.hpp"

namespace {

// Dwa napisy: jeden zawijany, drugi nie. Zmieniaj szerokość okna i patrz, który się łamie,
// a który znika pod krawędzią. F10 kończy.
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

    // Korzeń bierze jedno dziecko, bo nie jest układem. Układem jest kolumna.
    const auto column = std::make_shared<talcite::ui::Column>();

    sheet.children().add(column);

    column->children().add(std::make_shared<talcite::ui::Span>(
        "Ten napis się zawija, więc przy wąskim oknie rozłoży się na kilka wierszy, "
        "a przy szerokim zmieści się w jednym.",
        talcite::ui::SpanShape{true}));

    column->children().add(std::make_shared<talcite::ui::Span>(
        "Ten napis się nie zawija i po prostu zniknie pod krawędzią, gdy zabraknie miejsca."));

    column->children().add(std::make_shared<talcite::ui::Span>("F10 kończy."));

    term.show(sheet);
    term.title("talcite — tekst");

    term.run();

    return 0;
}
