#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "talcite/button.hpp"
#include "talcite/column.hpp"
#include "talcite/veilwindow.hpp"
#include "talcite/frame.hpp"
#include "talcite/legenditem.hpp"
#include "talcite/legendwindow.hpp"
#include "talcite/list.hpp"
#include "talcite/listitem.hpp"
#include "talcite/menu.hpp"
#include "talcite/menuitem.hpp"
#include "talcite/menuline.hpp"
#include "talcite/menuwindow.hpp"
#include "talcite/message.hpp"
#include "talcite/skin.hpp"
#include "talcite/span.hpp"
#include "talcite/term.hpp"

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

// Paleta odtwarzająca Midnight Commandera: granatowe panele, morski pasek menu, czerwone pudełko
// błędu. **Kod widżetów nie wie o niej nic** — pyta o role i dostaje to, co ta paleta mówi.
talcite::ui::Skin coloured() {
    const talcite::Color blue = tinted(0, 0, 160);
    const talcite::Color light = tinted(200, 200, 200);
    const talcite::Color white = tinted(255, 255, 255);
    const talcite::Color cyan = tinted(0, 160, 160);
    const talcite::Color black = tinted(0, 0, 0);
    const talcite::Color yellow = tinted(255, 230, 80);
    const talcite::Color red = tinted(190, 0, 0);

    return talcite::ui::Skin{{
        {"talcite.normal", painted(light, blue)},
        {"talcite.frame", painted(light, blue)},
        {"talcite.title", painted(yellow, blue)},

        {"talcite.list", painted(light, blue)},
        {"talcite.list.focus", painted(black, cyan)},

        {"talcite.menubar", painted(black, cyan)},
        {"talcite.menubar.focus", painted(black, light)},
        {"talcite.menu", painted(black, cyan)},
        {"talcite.menu.focus", painted(black, light)},
        {"talcite.menu.frame", painted(black, cyan)},

        {"talcite.legend.key", painted(light, black)},
        {"talcite.legend.text", painted(black, cyan)},

        // Zwykły komunikat: to samo okno co panel, tylko z własną rodziną ról.
        {"talcite.message", painted(light, blue)},
        {"talcite.message.title", painted(yellow, blue)},
        {"talcite.message.text", painted(white, blue)},
        {"talcite.message.box", painted(light, blue)},
        {"talcite.message.button", painted(black, cyan)},
        {"talcite.message.button.focus", painted(black, light)},
        {"talcite.message.button.key", painted(yellow, cyan)},

        // **Pudełko błędu.** Atrament równy papierowi w roli `talcite.error` gasi kreskę zewnętrznej
        // ramki — i to, co zostaje, jest kolumną oddechu wokół treści. Nie ma tu żadnego marginesu
        // w kodzie; jest krawędź ramki, której nie widać.
        {"talcite.error", painted(red, red)},
        {"talcite.error.title", painted(yellow, red)},
        {"talcite.error.text", painted(white, red)},
        {"talcite.error.box", painted(white, red)},
        {"talcite.error.button", painted(white, red)},
        {"talcite.error.button.focus", painted(white, red)},
        {"talcite.error.button.key", painted(yellow, red)},
    }};
}

}

// Powiadomienia jak w Midnight Commanderze. **Jedna klasa, cztery kształty**: komunikat z jednym
// przyciskiem, pytanie z kilkoma odpowiedziami, pudełko błędu i pudełko bez przycisków pokazywane
// na czas roboty. Różnicę robi liczba odpowiedzi i rdzeń roli — nic poza tym.
int main() {
    talcite::Term term;

    Sheet sheet{term};

    const auto legend = std::make_shared<talcite::ui::LegendWindow>();
    const auto menus = std::make_shared<talcite::ui::MenuWindow>();

    // **Klawisz otwierający podaje aplikacja** — biblioteka żadnego sobie nie przypisuje.
    menus->keys().add(talcite::ui::Key{talcite::ui::Code::f9});

    const auto dialogs = std::make_shared<talcite::ui::VeilWindow>();

    const auto frame = std::make_shared<talcite::ui::Frame>("/etc");
    const auto column = std::make_shared<talcite::ui::Column>();

    const auto told = std::make_shared<talcite::ui::Span>("F2 komunikat  F3 pytanie  F4 błąd  "
                                                       "F5 czekanie  F6 skórka");

    const auto list = std::make_shared<talcite::ui::List>(talcite::ui::plenty, talcite::ui::plenty,
                                                       talcite::ui::ListStyle{true});

    for (const char* one :
         {"..", "GNUstep", "ImageMagick-7", "ModemManager", "NetworkManager", "ODBCDataSources",
          "PackageKit", "UPower", "X11", "apt", "avahi", "bash_completion.d", "binfmt.d",
          "bluetooth", "byobu", "ca-certificates"})
        list->children().add(std::make_shared<talcite::ui::ListItem>(one));

    column->children().add(told);
    column->children().add(list);

    frame->children().add(column);
    dialogs->body()->children().add(frame);
    menus->body()->children().add(dialogs);
    legend->body()->children().add(menus);

    const auto file = menus->add("Plik");

    file->children().add(std::make_shared<talcite::ui::MenuItem>("Kopiuj", "F5"));
    file->children().add(std::make_shared<talcite::ui::MenuLine>());
    file->children().add(demo::doing(std::make_shared<talcite::ui::MenuItem>("Wyjście", "F10"),
                          [&term] { term.stop(); }));

    (void)menus->add("Widok");

    // **Czynność odpowiedzi siedzi na przycisku** — dokładnie tak samo jak w polu legendy i w
    // pozycji menu. Komunikat nie numeruje odpowiedzi i nie oddaje ich wołającemu; po naciśnięciu
    // po prostu się zamyka.
    //
    // „Zamknięto **bez** odpowiedzi" to osobna wiadomość, bo Esc nie ma przycisku, na którym
    // mogłaby wisieć czynność. Znacznik odróżnia jedno od drugiego, bo odpowiedź też kończy okno.
    const auto answering = [told](const std::shared_ptr<talcite::ui::Message>& one,
                                  std::vector<std::pair<std::string, char32_t>> these) {
        const auto answered = std::make_shared<bool>(false);

        // **Zamknięcie woła wołający**, bo jest częścią czynności odpowiedzi — komunikat sam
        // z ekranu nie schodzi. Uchwyt trzyma **słabe** wskazanie na okno: mocne zrobiłoby pętlę,
        // bo okno trzyma przycisk, a przycisk ten uchwyt.
        const std::weak_ptr<talcite::ui::Message> weak = one;

        for (const std::pair<std::string, char32_t>& what : these) {
            const std::shared_ptr<talcite::ui::Button> made = one->add(what.first);

            if (what.second != 0) made->keys().add(what.second);

            (void)demo::doing(made, [told, answered, weak, name = what.first] {
                *answered = true;

                told->text("odpowiedź: " + name);

                if (const auto kept = weak.lock()) kept->close();
            });
        }

        (void)one->on<talcite::ui::CloseEvent>([told, answered](talcite::ui::CloseEvent&) {
            if (!*answered) told->text("odpowiedź: Esc");

            return true;
        });
    };

    (void)demo::doing(legend->add(talcite::ui::Code::f2, "Komun"), [dialogs, answering] {
        const auto one =
            std::make_shared<talcite::ui::Message>("Uwaga", "Zapisano wszystkie zmiany.");

        answering(one, {{"OK", U'O'}});

        dialogs->open(demo::closing(one));
    });

    (void)demo::doing(legend->add(talcite::ui::Code::f3, "Pytanie"), [dialogs, answering] {
        const auto one = std::make_shared<talcite::ui::Message>(
            "Plik istnieje",
            "Plik „ca-certificates"
            "” już istnieje.\nCo z nim zrobić?");

        answering(one, {{"Nadpisz", U'N'}, {"Pomiń", U'P'}, {"Przerwij", U'z'}});

        one->buttons()[0]->primary(true);

        dialogs->open(demo::closing(one));
    });

    // **Błąd to ten sam widżet z innym rdzeniem.** Żadnej osobnej klasy, żadnej flagi w kodzie —
    // jedno wywołanie, a resztę mówi paleta.
    (void)demo::doing(legend->add(talcite::ui::Code::f4, "Błąd"), [dialogs, answering] {
        const auto one =
            std::make_shared<talcite::ui::Message>("Error", "Cannot operate on “..”!");

        one->role("talcite.error");

        answering(one, {{"OK", U'O'}});

        dialogs->open(demo::closing(one));
    });

    // Bez odpowiedzi: pudełko, które zamyka program, a nie użytkownik. Esc też je zamknie, bo to
    // wciąż dialog — od tego, żeby nie dało się go zamknąć, talcite nie ma i mieć nie będzie.
    (void)demo::doing(legend->add(talcite::ui::Code::f5, "Czekaj"), [dialogs] {
        dialogs->open(demo::closing(std::make_shared<talcite::ui::Message>("Czekaj", "Przeglądam katalog…")));
    });

    const auto plain = std::make_shared<bool>(false);

    (void)demo::doing(legend->add(talcite::ui::Code::f6, "Skórka"), [&sheet, plain] {
        *plain = !*plain;

        sheet.skin(*plain ? talcite::ui::Skin{} : coloured());
    });

    (void)demo::doing(legend->add(talcite::ui::Code::f9, "MenuGr"), [menus] {
        if (!menus->menus().empty()) menus->menus().front()->focus();
    });

    (void)demo::doing(legend->add(talcite::ui::Code::f10, "Wyjdź"), [&term] { term.stop(); });

    sheet.children().add(legend);
    sheet.skin(coloured());

    list->focus();

    term.onto(sheet);
    term.title("talcite — powiadomienia");

    term.run();

    return 0;
}
