#include <memory>
#include <string>

#include "talcite/button.hpp"
#include "talcite/column.hpp"
#include "talcite/field.hpp"
#include "talcite/frame.hpp"
#include "talcite/gauge.hpp"
#include "talcite/legenditem.hpp"
#include "talcite/legend.hpp"
#include "talcite/line.hpp"
#include "talcite/list.hpp"
#include "talcite/listitem.hpp"
#include "talcite/menu.hpp"
#include "talcite/menuitem.hpp"
#include "talcite/menuseparator.hpp"
#include "talcite/menubar.hpp"
#include "talcite/row.hpp"
#include "talcite/skin.hpp"
#include "talcite/span.hpp"
#include "talcite/term.hpp"

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

talcite::Style painted(talcite::Color foreground, talcite::Color background) {
    talcite::Style made{};

    made.foreground = foreground;
    made.background = background;

    return made;
}

talcite::Color tinted(std::uint8_t red, std::uint8_t green, std::uint8_t blue) {
    return talcite::Color{false, red, green, blue};
}

// Skórka barwna: te same role, inne wartości. Żaden widżet o niej nie wie — pyta o rolę i dostaje
// to, co ta skórka mówi.
talcite::ui::Skin coloured() {
    const talcite::Color background = tinted(0, 0, 140);
    const talcite::Color light = tinted(200, 200, 200);
    const talcite::Color white = tinted(255, 255, 255);
    const talcite::Color cyan = tinted(0, 160, 160);
    const talcite::Color black = tinted(0, 0, 0);
    const talcite::Color yellow = tinted(255, 230, 80);

    return talcite::ui::Skin{{
        {"talcite.normal", painted(light, background)},      {"talcite.frame", painted(light, background)},
        {"talcite.title", painted(yellow, background)},

        {"talcite.list", painted(light, background)},        {"talcite.list.focus", painted(black, cyan)},
        {"talcite.list.mark", painted(white, background)},

        {"talcite.menubar", painted(black, light)},     {"talcite.menubar.focus", painted(black, cyan)},
        {"talcite.menu", painted(black, light)},        {"talcite.menu.focus", painted(black, cyan)},
        {"talcite.menu.frame", painted(black, light)},

        {"talcite.legend.key", painted(light, black)},  {"talcite.legend.text", painted(black, cyan)},

        {"talcite.input", painted(black, cyan)},        {"talcite.button", painted(black, light)},
        {"talcite.button.focus", painted(black, cyan)}, {"talcite.button.key", painted(yellow, light)},

        {"talcite.scroll", painted(light, background)},      {"talcite.scroll.thumb", painted(cyan, background)},

        {"talcite.gauge", painted(light, background)},       {"talcite.gauge.done", painted(cyan, background)},
        {"talcite.gauge.text", painted(yellow, background)},
    }};
}

// Skórka dla terminala bez znaków rysunkowych. **To jest cały zastępnik ASCII** — nie gałąź w
// kodzie, tylko druga paleta.
talcite::ui::Skin plainly() {
    return talcite::ui::Skin{{},
                          {
                              {"talcite.line.thin.across", U'-'},
                              {"talcite.line.thin.upright", U'|'},
                              {"talcite.line.thin.upper.left", U'+'},
                              {"talcite.line.thin.upper.right", U'+'},
                              {"talcite.line.thin.lower.left", U'+'},
                              {"talcite.line.thin.lower.right", U'+'},
                              {"talcite.line.thin.left.joint", U'+'},
                              {"talcite.line.thin.right.joint", U'+'},

                              {"talcite.scroll.up", U'^'},
                              {"talcite.scroll.down", U'v'},
                              {"talcite.scroll.thumb", U'#'},
                              {"talcite.scroll.track", U'.'},

                              {"talcite.gauge.done", U'#'},
                              {"talcite.gauge.rest", U'.'},

                              {"talcite.elide", U'~'},
                          }};
}

}

// Jeden ekran, trzy palety. **Przełączenie to jedno wywołanie na korzeniu** — bez powiadamiania,
// bez obchodzenia drzewa i bez przebudowy czegokolwiek: następna klatka po prostu czyta co innego.
int main() {
    talcite::Term term;

    Sheet sheet{term};

    const auto legend = std::make_shared<talcite::ui::Legend>();
    const auto menus = std::make_shared<talcite::ui::Menubar>();

    // **Klawisz otwierający podaje aplikacja** — biblioteka żadnego sobie nie przypisuje.
    menus->keys().add(talcite::ui::Key{talcite::ui::Code::f9});


    const auto frame = std::make_shared<talcite::ui::Frame>("talcite — skórki");
    const auto column = std::make_shared<talcite::ui::Column>();

    const auto told = std::make_shared<talcite::ui::Span>("skórka wbudowana");

    const auto list =
        std::make_shared<talcite::ui::List>(talcite::ui::plenty, 6, talcite::ui::ScrollShape{true});

    for (int which = 0; which < 30; ++which)
        list->children().add(std::make_shared<talcite::ui::ListItem>("bardzo długa nazwa pliku numer " +
                                                       std::to_string(which) + ".txt"));

    const auto field = std::make_shared<talcite::ui::Field>(30, "nazwa");

    const auto gauge = std::make_shared<talcite::ui::Gauge>(40);

    gauge->part(0.62);

    const auto yes = std::make_shared<talcite::ui::Button>("Zmień");
    const auto no = std::make_shared<talcite::ui::Button>("Przerwij");

    yes->keys().add(U'Z');
    no->keys().add(U'P');
    yes->primary(true);

    const auto row = std::make_shared<talcite::ui::Row>();

    row->children().add(yes);
    row->children().add(no);

    column->children().add(told);
    column->children().add(std::make_shared<talcite::ui::Line>("Pliki"));
    column->children().add(list);
    column->children().add(std::make_shared<talcite::ui::Line>("Nazwa"));
    column->children().add(field);
    column->children().add(gauge);
    column->children().add(row);

    frame->children().add(column);
    menus->content(frame);
    legend->content(menus);

    const auto file = menus->add("Plik");

    file->children().add(std::make_shared<talcite::ui::MenuItem>("Kopiuj", "F5"));
    file->children().add(std::make_shared<talcite::ui::MenuSeparator>());
    file->children().add(demo::doing(std::make_shared<talcite::ui::MenuItem>("Wyjście", "F10"),
                          [&term] { term.stop(); }));

    (void)menus->add("Widok");

    (void)demo::doing(legend->add(talcite::ui::Code::f2, "Zwykła"), [&sheet, told] {
        sheet.skin(talcite::ui::Skin{});
        told->text("skórka wbudowana");
    });

    (void)demo::doing(legend->add(talcite::ui::Code::f3, "Barwna"), [&sheet, told] {
        sheet.skin(coloured());
        told->text("skórka barwna — te same role, inne wartości");
    });

    (void)demo::doing(legend->add(talcite::ui::Code::f4, "ASCII"), [&sheet, told] {
        sheet.skin(plainly());
        told->text("skórka ASCII — same znaki, bez rysunkowych");
    });

    (void)demo::doing(legend->add(talcite::ui::Code::f9, "MenuGr"), [menus] {
        if (!menus->menus().empty()) menus->menus().front()->focus();
    });

    (void)demo::doing(legend->add(talcite::ui::Code::f10, "Wyjdź"), [&term] { term.stop(); });

    sheet.children().add(legend);

    list->focus();

    term.show(sheet);
    term.title("talcite — skórki");

    term.run();

    return 0;
}
