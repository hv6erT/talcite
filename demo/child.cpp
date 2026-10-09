#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "talcite/child.hpp"
#include "talcite/column.hpp"
#include "talcite/field.hpp"
#include "talcite/frame.hpp"
#include "talcite/legenditem.hpp"
#include "talcite/legend.hpp"
#include "talcite/promise.hpp"
#include "talcite/skin.hpp"
#include "talcite/span.hpp"
#include "talcite/term.hpp"
#include "talcite/viewer.hpp"

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

talcite::Color tinted(std::uint8_t red, std::uint8_t green, std::uint8_t blue) {
    return talcite::Color{false, red, green, blue};
}

talcite::Style painted(talcite::Color foreground, talcite::Color background) {
    talcite::Style made{};

    made.foreground = foreground;
    made.background = background;

    return made;
}

talcite::ui::Skin coloured() {
    const talcite::Color blue = tinted(0, 0, 160);
    const talcite::Color light = tinted(200, 200, 200);
    const talcite::Color cyan = tinted(0, 160, 160);
    const talcite::Color black = tinted(0, 0, 0);
    const talcite::Color yellow = tinted(255, 230, 80);

    return talcite::ui::Skin{{
        {"talcite.normal", painted(light, blue)},
        {"talcite.frame", painted(light, blue)},
        {"talcite.title", painted(yellow, blue)},
        {"talcite.page", painted(light, blue)},
        {"talcite.field", painted(black, cyan)},
        {"talcite.field.focus", painted(black, light)},
        {"talcite.legend.key", painted(light, black)},
        {"talcite.legend.text", painted(black, cyan)},
    }};
}

// Powłoka jest **tym, co uruchamiamy**, a nie tym, co talcite o czymkolwiek zakłada: to demonstracja
// wpisuje `sh -c`, bo chce podać całe zdanie, i to ona wie, jak się ta powłoka nazywa na jej
// systemie. Biblioteka dostaje zwykły przepis z programem i argumentami.
talcite::Command spoken(const std::string& said) {
    talcite::Command made;

#ifdef _WIN32

    made.program = talcite::fs::path{"cmd.exe"};
    made.arguments.emplace_back("/c");

#else

    made.program = talcite::fs::path{"/bin/sh"};
    made.arguments.emplace_back("-c");

#endif

    made.arguments.push_back(said);

    return made;
}

talcite::Bytes joined(talcite::Bytes read, const talcite::Bytes& errors) {
    read.insert(read.end(), errors.begin(), errors.end());

    return read;
}

}

// **Uruchamianie cudzych programów, dwiema drogami.** Enter zbiera wyjście do podglądu — polecenie
// biegnie obok, a okno przez ten czas żyje normalnie. F4 oddaje programowi cały terminal i czeka:
// tak w Midnight Commanderze otwiera się edytor albo `less`, i tak jedynie mogą działać programy,
// które same rysują po ekranie.
int main() {
    talcite::Term term;

    Sheet sheet{term};

    const auto legend = std::make_shared<talcite::ui::Legend>();
    const auto frame = std::make_shared<talcite::ui::Frame>("wyjście");
    const auto column = std::make_shared<talcite::ui::Column>();

    const auto told = std::make_shared<talcite::ui::Span>("Enter — zbierz wyjście   F4 — oddaj ekran");
    const auto viewer = std::make_shared<talcite::ui::Viewer>();
    const auto field = std::make_shared<talcite::ui::Field>(talcite::ui::plenty, "talcite --wersja");

    frame->children().add(viewer);

    column->children().add(told);
    column->children().add(frame);
    column->children().add(field);

    legend->content(column);

    const auto showing = [told, viewer](const talcite::Output& made) {
        viewer->hold(joined(made.out, made.err));

        told->text("kod wyjścia: " + std::to_string(made.exit.code));
    };

    // **Zbieranie biegnie obok.** `here()` przenosi ciąg dalszy na pętlę rysującą, więc dotykanie
    // drzewa węzłów odbywa się na tym samym wątku, co rysowanie — a przez ten czas okno reaguje.
    //
    // Obietnicę trzymamy, bo ogniwo wisi na niej słabym wskazaniem: porzucona nigdy by nie
    // zadziałała. I trzeba ją ruszyć — same `then` niczego nie uruchamia.
    const auto kept = std::make_shared<std::optional<talcite::Promise<void>>>();

    const auto gathering = [field, told, showing, kept] {
        told->text("biegnie…");

        kept->emplace(talcite::Child::run(spoken(field->text()))
                          .here()
                          .then([showing](const talcite::Output& made) { showing(made); })
                          .fail([told](const std::exception& why) {
                              told->text(std::string{"nie poszło: "} + why.what());
                          }));

        (*kept)->start();
    };

    (void)field->on<talcite::ui::KeyEvent>([gathering](talcite::ui::KeyEvent& what) {
        if (what.key() != talcite::ui::Code::enter) return false;

        gathering();

        return true;
    });

    (void)demo::doing(legend->add(talcite::ui::Code::f3, "Zbierz"), gathering);

    // Oddanie ekranu jest **zwykłym wywołaniem, które trwa**: pętla rysująca stoi, bo terminal
    // należy teraz do kogoś innego. Wątki puli — zegar, sieć, czytanie plików — biegną dalej.
    (void)demo::doing(legend->add(talcite::ui::Code::f4, "Oddaj"), [&term, field, told] {
        const talcite::Exit made = term.execute(spoken(field->text()));

        told->text("wrócił z kodem " + std::to_string(made.code));
    });

    (void)demo::doing(legend->add(talcite::ui::Code::f10, "Wyjdź"), [&term] { term.stop(); });

    sheet.children().add(legend);
    sheet.skin(coloured());

    field->focus();

    term.show(sheet);
    term.title("talcite — cudze programy");

    term.run();

    return 0;
}
