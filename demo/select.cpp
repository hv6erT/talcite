#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "talcite/column.hpp"
#include "talcite/dialog.hpp"
#include "talcite/encoding.hpp"
#include "talcite/frame.hpp"
#include "talcite/legenditem.hpp"
#include "talcite/legendwindow.hpp"
#include "talcite/row.hpp"
#include "talcite/select.hpp"
#include "talcite/skin.hpp"
#include "talcite/span.hpp"
#include "talcite/term.hpp"
#include "talcite/veilwindow.hpp"

#include "doing.hpp"

namespace {

class Sheet : public talcite::ui::Root {
public:
    explicit Sheet(talcite::Term& term) : term_(term) {}

protected:
    bool react(talcite::ui::KeyEvent& what) override {
        if (what.key() == talcite::ui::Code::f10) term_.stop();

        return true;
    }

private:
    talcite::Term& term_;
};

talcite::Color tinted(std::uint8_t red, std::uint8_t green, std::uint8_t blue) {
    return talcite::Color{false, red, green, blue};
}

talcite::Style painted(talcite::Color ink, talcite::Color paper) {
    talcite::Style made{};

    made.ink = ink;
    made.paper = paper;

    return made;
}

// Paleta Midnight Commandera. Wybór dostaje własną rodzinę ról — **kod widżetu nie wie o niej nic**,
// pyta o role i dostaje to, co ta paleta mówi.
talcite::ui::Skin coloured() {
    const talcite::Color blue = tinted(0, 0, 160);
    const talcite::Color light = tinted(200, 200, 200);
    const talcite::Color white = tinted(255, 255, 255);
    const talcite::Color cyan = tinted(0, 160, 160);
    const talcite::Color black = tinted(0, 0, 0);
    const talcite::Color yellow = tinted(255, 230, 80);

    return talcite::ui::Skin{{
        {"talcite.normal", painted(light, blue)},
        {"talcite.frame", painted(light, blue)},
        {"talcite.title", painted(yellow, blue)},

        {"talcite.select", painted(black, cyan)},
        {"talcite.select.focus", painted(black, light)},
        {"talcite.select.off", painted(blue, cyan)},

        {"talcite.list", painted(black, cyan)},
        {"talcite.list.focus", painted(black, light)},

        {"talcite.legend.key", painted(light, black)},
        {"talcite.legend.text", painted(black, cyan)},

        {"talcite.message", painted(white, blue)},
        {"talcite.message.title", painted(yellow, blue)},
        {"talcite.message.text", painted(white, blue)},
    }};
}

std::vector<std::string> encodings() {
    return {"UTF-8",   "ISO-8859-2",   "CP1250",   "CP852",  "KOI8-R",
            "Big5",    "Shift_JIS",    "EUC-KR",   "IBM866", "MacRoman"};
}

// Wiersz ustawienia: podpis po lewej, wybór po prawej. Wiersz daje każdemu dziecku tyle, ile
// dziecko chce, więc podpisy same się nie wyrównują — od tego jest dopełnienie do stałej szerokości.
//
// Dopełniamy do **kratek**, nie do bajtów: „Końce wierszy" ma trzynaście znaków i piętnaście bajtów,
// więc liczone bajtami wyszłoby o dwie spacje za krótkie.
std::shared_ptr<talcite::ui::Row> setting(const std::string& said,
                                        const std::shared_ptr<talcite::ui::Select>& one) {
    const auto row = std::make_shared<talcite::ui::Row>();

    const int wide = talcite::measured(said).wide;
    const auto label =
        std::make_shared<talcite::ui::Span>(said + std::string(std::max(16 - wide, 1), ' '));

    row->children().add(label);
    row->children().add(one);

    return row;
}

}

// **Lista rozwijana.** Zamknięta jest jednym wierszem z wartością i strzałką; rozwinięta pokazuje
// resztę w pudełku pod sobą — a gdy pod spodem brakuje miejsca, nad sobą. Pudełko leży na wszystkim,
// bo jest zasłoną w tym samym oknie, co dialogi.
int main() {
    talcite::Term term;

    Sheet sheet{term};

    const auto legend = std::make_shared<talcite::ui::LegendWindow>();
    const auto veils = std::make_shared<talcite::ui::VeilWindow>();

    const auto frame = std::make_shared<talcite::ui::Frame>("Ustawienia");
    const auto column = std::make_shared<talcite::ui::Column>();

    const auto told = std::make_shared<talcite::ui::Span>(
        "Enter rozwija, strzałki przestawiają bez rozwijania.");

    const auto coding = std::make_shared<talcite::ui::Select>(encodings());
    const auto ending = std::make_shared<talcite::ui::Select>(
        std::vector<std::string>{"LF (Unix)", "CRLF (Windows)", "CR (klasyczny Mac)"});
    const auto sorting = std::make_shared<talcite::ui::Select>(
        std::vector<std::string>{"nazwa", "rozszerzenie", "rozmiar", "czas zapisu", "bez sortowania"});

    const auto said = std::make_shared<talcite::ui::Span>("");

    column->children().add(told);
    column->children().add(std::make_shared<talcite::ui::Span>(""));
    column->children().add(setting("Kodowanie", coding));
    column->children().add(setting("Końce wierszy", ending));
    column->children().add(setting("Sortowanie", sorting));
    column->children().add(std::make_shared<talcite::ui::Span>(""));
    column->children().add(said);

    frame->children().add(column);
    veils->body()->children().add(frame);
    legend->body()->children().add(veils);

    // Każdy wybór ogłasza **to samo zdarzenie**, niezależnie od tego, czy wybrano myszą, Enterem
    // czy strzałką w zamkniętym wierszu.
    const auto watching = [said](const std::shared_ptr<talcite::ui::Select>& one,
                                 const std::string& named) {
        (void)one->on<talcite::ui::PickEvent>([said, one, named](talcite::ui::PickEvent&) {
            said->text(named + ": " + one->which().value_or(std::string{}));

            return true;
        });
    };

    watching(coding, "Kodowanie");
    watching(ending, "Końce wierszy");
    watching(sorting, "Sortowanie");

    (void)demo::doing(legend->add(talcite::ui::Code::f2, "Podaj"), [coding, said] {
        // **Ustawienie z programu milczy**: wypełnienie okna wartościami
        // początkowymi nie jest wybieraniem.
        // Numer opcji podaje sam zbiór — na zewnątrz wybór mówi wyłącznie napisami.
        const int at = coding->options().which(coding->which().value_or(std::string{}));

        const std::size_t next =
            static_cast<std::size_t>(at + 1) % std::max(coding->options().many(), std::size_t{1});

        coding->which(coding->options()[next]);

        said->text("podano z programu (bez zdarzenia): " +
                   coding->which().value_or(std::string{}));
    });

    (void)demo::doing(legend->add(talcite::ui::Code::f10, "Wyjdź"), [&term] { term.stop(); });

    sheet.children().add(legend);
    sheet.skin(coloured());

    coding->focus();

    term.onto(sheet);
    term.title("talcite — lista rozwijana");

    term.run();

    return 0;
}
