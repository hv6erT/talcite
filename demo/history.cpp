#include <cstdint>
#include <memory>
#include <string>

#include "talcite/column.hpp"
#include "talcite/frame.hpp"
#include "talcite/historyfield.hpp"
#include "talcite/legenditem.hpp"
#include "talcite/legend.hpp"
#include "talcite/list.hpp"
#include "talcite/listitem.hpp"
#include "talcite/row.hpp"
#include "talcite/skin.hpp"
#include "talcite/span.hpp"
#include "talcite/term.hpp"
#include "talcite/modals.hpp"

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

        {"talcite.field", painted(black, cyan)},
        {"talcite.field.focus", painted(black, light)},

        {"talcite.list", painted(light, blue)},
        {"talcite.list.focus", painted(black, cyan)},

        {"talcite.legend.key", painted(light, black)},
        {"talcite.legend.text", painted(black, cyan)},
    }};
}

}

// **Wiersz poleceń z historią**, jak pod panelami Midnight Commandera. Strzałki chodzą po przeszłych
// wpisach w miejscu, Alt+H rozwija je listą pod polem.
//
// Zauważ, kto dokłada wpisy: **aplikacja, nie widżet**. Pole nie wie, kiedy polecenie zostało
// przyjęte — Enter w oknie naciska przycisk podstawowy, a nie zatwierdza wiersza. Wie to tylko ten,
// kto składa okno, i to on mówi `add`.
int main() {
    talcite::Term term;

    Sheet sheet{term};

    const auto legend = std::make_shared<talcite::ui::Legend>();
    const auto modals = std::make_shared<talcite::ui::Modals>();

    const auto column = std::make_shared<talcite::ui::Column>();
    const auto frame = std::make_shared<talcite::ui::Frame>("Wykonane");
    const auto done = std::make_shared<talcite::ui::List>(talcite::ui::plenty, talcite::ui::plenty);

    const auto told = std::make_shared<talcite::ui::Span>(
        "Enter wykonuje, strzałki wędrują po historii, Alt+H rozwija ją listą.");

    const auto line = std::make_shared<talcite::ui::Row>();
    const auto sign = std::make_shared<talcite::ui::Span>("Polecenie: ");
    const auto said = std::make_shared<talcite::ui::HistoryField>(talcite::ui::plenty);

    // **Klawisz rozwijający podaje aplikacja**, bo biblioteka żadnego sobie nie przypisuje — i leży
    // tam, gdzie klawisze wszystkich innych widżetów, czyli w `keys()`. Modyfikator jest częścią
    // klawisza, więc „h" bez Altu dalej jest zwykłą literą do wpisania.
    {
        talcite::ui::Mods alt;

        alt.alt = true;

        said->keys().add(talcite::ui::Key{U'h', alt});
    }

    // Historia wczytana ze „starego pliku ustawień" — dokładana wpis po wpisie, więc przechodzi
    // przez odsiewanie powtórzeń i przez sufit tak samo jak każda inna.
    for (const char* one :
         {"make -j8", "cmake --build out", "git status", "grep -rn TODO src", "ls -la /etc"})
        said->options().insert(0, one);

    said->text("");

    line->children().add(sign);
    line->children().add(said);

    frame->children().add(done);

    // **Wiersz poleceń stoi nad wykazem, nie pod nim** — i to nie jest upodobanie. Kolumna daje
    // dzieciom po kolei tyle, ile chcą, więc ten, kto poprosi o wszystko, zabiera resztę: ramka
    // z wykazem postawiona przed wierszem poleceń zepchnęłaby go poza ekran.
    //
    // W Midnight Commanderze wiersz stoi pod panelami, bo pasek klawiszy rezerwuje sobie ostatni
    // wiersz osobnym oknem. Układu „przypnij do dołu" talcite jeszcze nie ma.
    column->children().add(told);
    column->children().add(line);
    column->children().add(frame);

    modals->content(column);
    legend->content(modals);

    // Enter nie należy do pola — pole go przepuszcza, więc łapiemy go tutaj i to jest miejsce,
    // w którym polecenie „się liczy".
    (void)said->on<talcite::ui::KeyEvent>([said, done](talcite::ui::KeyEvent& what) {
        if (what.key() != talcite::ui::Code::enter) return false;

        if (said->text().empty()) return true;

        done->children().add(std::make_shared<talcite::ui::ListItem>(said->text()));

        said->options().insert(0, said->text());
        said->text("");

        return true;
    });

    // Wzięcie wpisu z przeszłości ogłasza **jedno** zdarzenie, wszystko jedno czy strzałką, czy
    // z rozwiniętej listy.
    (void)said->on<talcite::ui::PickEvent>([said, told](talcite::ui::PickEvent&) {
        told->text("wzięte z historii: " + said->text());

        return true;
    });

    (void)demo::doing(legend->add(talcite::ui::Code::f3, "Historia"),
                      [said] { said->open(); });

    (void)demo::doing(legend->add(talcite::ui::Code::f8, "Zapomnij"), [said, told] {
        said->clear();

        told->text("historia wyczyszczona");
    });

    (void)demo::doing(legend->add(talcite::ui::Code::f10, "Wyjdź"), [&term] { term.stop(); });

    sheet.children().add(legend);
    sheet.skin(coloured());

    said->focus();

    term.show(sheet);
    term.title("talcite — historia poleceń");

    term.run();

    return 0;
}
