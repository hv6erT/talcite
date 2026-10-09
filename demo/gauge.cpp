#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "talcite/column.hpp"
#include "talcite/frame.hpp"
#include "talcite/gauge.hpp"
#include "talcite/legenditem.hpp"
#include "talcite/legend.hpp"
#include "talcite/line.hpp"
#include "talcite/promise.hpp"
#include "talcite/span.hpp"
#include "talcite/term.hpp"
#include "talcite/time.hpp"

#include "doing.hpp"

namespace {

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

// Dwa paski: jeden wie, ile zrobił, drugi tylko tyle, że coś się dzieje. Ten drugi kręci się sam
// — zegarem biblioteki, bez ani jednej linijki po stronie programu.
int main() {
    talcite::Term term;

    Sheet sheet{term};

    const auto legend = std::make_shared<talcite::ui::Legend>();
    const auto frame = std::make_shared<talcite::ui::Frame>("talcite — pasek postępu");
    const auto column = std::make_shared<talcite::ui::Column>();

    const auto told = std::make_shared<talcite::ui::Span>("F5 zaczyna, F6 przełącza nieokreślony");

    const auto known = std::make_shared<talcite::ui::Gauge>(46);
    const auto guess = std::make_shared<talcite::ui::Gauge>(46);

    guess->busy(true);

    column->children().add(told);
    column->children().add(std::make_shared<talcite::ui::Line>("Kopiowanie"));
    column->children().add(known);
    column->children().add(std::make_shared<talcite::ui::Line>("Czekanie"));
    column->children().add(guess);

    frame->children().add(column);
    legend->content(frame);

    // Postęp udawany: co pięćdziesiąt milisekund o krok dalej. W prawdziwym programie zamiast
    // licznika stoi tu `one.progress()` z obietnicy kopiowania — ta sama liczba, ten sam ułamek.
    const auto running = std::make_shared<bool>(false);
    const auto again = std::make_shared<std::function<void()>>();

    // **Obietnicę trzeba trzymać do rozstrzygnięcia.** Porzucone `then(...).start()` nigdy nie
    // zadziała, bo ogniwo wisi na obietnicy słabym wskazaniem — to ta sama reguła, dzięki której
    // śmierć właściciela odcina wywołania zwrotne.
    const auto beat = std::make_shared<std::optional<talcite::Promise<void>>>();

    *again = [known, told, running, again, beat] {
        if (!*running) return;

        const double next = known->part() + 0.01;

        known->part(next);

        if (next >= 1.0) {
            *running = false;

            told->text("skończone");

            return;
        }

        beat->emplace(
            talcite::Time::later(std::chrono::milliseconds{50}).here().then([again] { (*again)(); }));

        (*beat)->start();
    };

    (void)demo::doing(legend->add(talcite::ui::Code::f5, "Zacznij"),
                      [known, told, running, again] {
                          known->part(0.0);
                          told->text("kopiowanie…");

                          if (*running) return;

                          *running = true;

                          (*again)();
                      });

    (void)demo::doing(legend->add(talcite::ui::Code::f6, "Czekaj"),
                      [guess] { guess->busy(!guess->busy()); });

    (void)demo::doing(legend->add(talcite::ui::Code::f10, "Wyjdź"), [&term] { term.stop(); });

    sheet.children().add(legend);

    term.show(sheet);
    term.title("talcite — pasek postępu");

    term.run();

    return 0;
}
