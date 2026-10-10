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
    // F10 stops the terminal; every other key is reported as handled.
    bool handle(talcite::ui::KeyEvent& what) override {
        if (what.key() == talcite::ui::Code::f10) term_.stop();

        return true;
    }

private:
    talcite::Term& term_;
};

}

// Two bars: one knows how much it has done, the other only that something is happening. The second
// one spins by itself, on the library's clock, without a single line on the program's side.
int main() {
    talcite::Term term;

    Sheet sheet{term};

    const auto legend = std::make_shared<talcite::ui::Legend>();
    const auto frame = std::make_shared<talcite::ui::Frame>("talcite — progress bar");
    const auto column = std::make_shared<talcite::ui::Column>();

    const auto told = std::make_shared<talcite::ui::Span>("F5 starts, F6 toggles the indeterminate bar");

    const auto known = std::make_shared<talcite::ui::Gauge>(46);
    const auto guess = std::make_shared<talcite::ui::Gauge>(46);

    guess->busy(true);

    column->children().add(told);
    column->children().add(std::make_shared<talcite::ui::Line>("Copying"));
    column->children().add(known);
    column->children().add(std::make_shared<talcite::ui::Line>("Waiting"));
    column->children().add(guess);

    frame->children().add(column);
    legend->content(frame);

    // Pretend progress: one step further every fifty milliseconds. In a real program, instead of
    // the counter, this would be `one.progress()` from the copy promise: the same number, the same
    // fraction.
    const auto running = std::make_shared<bool>(false);
    const auto again = std::make_shared<std::function<void()>>();

    // The promise has to be kept until it settles. An abandoned `then(...).start()` never fires,
    // because the chain hangs on the promise through a weak pointer; it is the same rule that cuts
    // off the callbacks when their owner dies.
    const auto beat = std::make_shared<std::optional<talcite::Promise<void>>>();

    *again = [known, told, running, again, beat] {
        if (!*running) return;

        const double next = known->part() + 0.01;

        known->part(next);

        if (next >= 1.0) {
            *running = false;

            told->text("done");

            return;
        }

        beat->emplace(
            talcite::Time::later(std::chrono::milliseconds{50}).here().then([again] { (*again)(); }));

        (*beat)->start();
    };

    (void)demo::doing(legend->add(talcite::ui::Code::f5, "Start"),
                      [known, told, running, again] {
                          known->part(0.0);
                          told->text("copying…");

                          if (*running) return;

                          *running = true;

                          (*again)();
                      });

    (void)demo::doing(legend->add(talcite::ui::Code::f6, "Wait"),
                      [guess] { guess->busy(!guess->busy()); });

    (void)demo::doing(legend->add(talcite::ui::Code::f10, "Quit"), [&term] { term.stop(); });

    sheet.children().add(legend);

    term.show(sheet);
    term.title("talcite — progress bar");

    term.run();

    return 0;
}
